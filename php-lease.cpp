#include "PHP/php-lease.h"

#include "common/options.h"
#include "common/precise-time.h"
#include "common/rpc-const.h"
#include "net/net-connections.h"
#include "net/net-sockaddr-storage.h"

#include "PHP/php-engine-vars.h"
#include "PHP/php-ready.h"
#include "PHP/php-worker.h"

static int rpc_main_target = -1;

static int cur_lease_target_ip = -1;
static int cur_lease_target_port = -1;
static conn_target_t *cur_lease_target_ct = nullptr;
static int cur_lease_target = -1;

static int rpc_lease_target = -1;
static double rpc_lease_timeout = -1;
static process_id_t lease_pid;

static double lease_stats_start_time;
static double lease_stats_time;
static long long lease_stats_cnt;
static int ready_cnt = 0;

enum lease_state_t {
  lst_off,            // connect to rpc-proxy, wait kphp.startLease from it, connect to target
  lst_start,          // if !has_pending_scripts -> change state to lst_on, wait for connection to target ready, do lease_set_ready() && run_rpc_lease();
  lst_on,             // connecting to target, send RPC_READY, doing work, if too much time doing work -> change state to lst_finish
  lst_finish          // wait finishing current task, send RPC_STOP to target, send TL_KPHP_LEASE_STATS to rpc-proxy, change state to lst_off
};

static lease_state_t lease_state = lst_off;
static int lease_ready_flag = 0; // waiting for something -> equal 0

extern conn_target_t rpc_client_ct;

int get_target_by_pid(int ip, int port, conn_target_t *ct);
void send_rpc_query(connection *c, int op, long long id, int *q, int qsize);
connection *get_target_connection(conn_target_t *S, int force_flag);
int has_pending_scripts();

static int get_lease_target_by_pid(int ip, int port, conn_target_t *ct) {
  if (ip == cur_lease_target_ip && port == cur_lease_target_port && ct == cur_lease_target_ct) {
    return cur_lease_target;
  }
  if (cur_lease_target != -1) {
    conn_target_t *old_target = &Targets[cur_lease_target];
    destroy_target(old_target);
  }
  cur_lease_target_ip = ip;
  cur_lease_target_port = port;
  cur_lease_target_ct = ct;
  cur_lease_target = get_target_by_pid(ip, port, ct);

  return cur_lease_target;
}

static void lease_change_state(lease_state_t new_state) {
  if (lease_state != new_state) {
    lease_state = new_state;
    lease_ready_flag = 0;
  }
}

void lease_on_worker_finish(php_worker *worker) {
  if ((lease_state == lst_on || lease_state == lst_finish) && worker->target_fd == rpc_lease_target) {
    double worked = precise_now - worker->start_time;
    lease_stats_time += worked;
    lease_stats_cnt++;
  }
}

static void rpc_send_stopped(connection *c) {
  int q[100], qn = 0;
  qn += 2;
  q[qn++] = -1;
  q[qn++] = (int)inet_sockaddr_address(&c->local_endpoint);
  q[qn++] = (int)inet_sockaddr_port(&c->local_endpoint);
  q[qn++] = pid; // pid
  q[qn++] = now - get_uptime(); // start_time
  q[qn++] = worker_id; // id
  q[qn++] = ready_cnt++; // ready_cnt
  qn++;
  send_rpc_query(c, TL_KPHP_STOP_READY, -1, q, qn * 4);
}

static void rpc_send_lease_stats(connection *c) {
  int q[100], qn = 0;
  qn += 2;
  q[qn++] = -1;
  *(process_id_t *)(q + qn) = lease_pid;
  static_assert(sizeof(lease_pid) == 12, "");
  qn += 3;
  *(double *)(q + qn) = precise_now - lease_stats_start_time;
  qn += 2;
  *(double *)(q + qn) = lease_stats_time;
  qn += 2;
  q[qn++] = lease_stats_cnt;
  qn++;

  send_rpc_query(c, TL_KPHP_LEASE_STATS, -1, q, qn * 4);
}

static int is_staging;
FLAG_OPTION_PARSER(OPT_ENGINE_CUSTOM, "staging", is_staging, "kphp sends this info to tasks if running as worker");

static void rpc_send_ready(connection *c) {
  int q[100], qn = 0;
  qn += 2;
  bool use_ready_v2 = (is_staging != 0);
  int magic = use_ready_v2 ? TL_KPHP_READY_V2 : TL_KPHP_READY;

  q[qn++] = -1; // will be replaced by op
  if (get_current_target() == rpc_main_target && rpc_client_actor != -1) { // we don't want to update all tasks
    *reinterpret_cast<long long*>(&q[qn]) = 0;
    qn += 2;
    q[qn++] = TL_RPC_DEST_ACTOR;
    *reinterpret_cast<long long*>(&q[qn]) = rpc_client_actor;
    qn += 2;
    q[qn++] = magic;
    magic = RPC_INVOKE_REQ;
  }
  if (use_ready_v2) {
    q[qn++] = is_staging ? KPHP_READY_FIELDS_MASK_STAGING : 0;
  }
  q[qn++] = static_cast<int>(inet_sockaddr_address(&c->local_endpoint));
  q[qn++] = static_cast<int>(inet_sockaddr_port(&c->local_endpoint));
  q[qn++] = pid; // pid
  q[qn++] = now - get_uptime(); // start_time
  q[qn++] = worker_id; // id
  q[qn++] = ready_cnt++; // ready_cnt
  qn++;
  send_rpc_query(c, magic, -1, q, qn * 4);
}


static int rpct_ready(int target_fd) {
  if (target_fd == -1) {
    return -1;
  }
  conn_target_t *target = &Targets[target_fd];
  connection *conn = get_target_connection(target, 0);
  if (conn == nullptr) {
    return -2;
  }
  rpc_send_ready(conn);
  return 0;
}

static void rpct_stop_ready(int target_fd) {
  if (target_fd == -1) {
    return;
  }
  conn_target_t *target = &Targets[target_fd];
  connection *conn = get_target_connection(target, 0);
  if (conn != nullptr) {
    rpc_send_stopped(conn);
  }
}

static void rpct_lease_stats(int target_fd) {
  if (target_fd == -1) {
    return;
  }
  conn_target_t *target = &Targets[target_fd];
  connection *conn = get_target_connection(target, 0);
  if (conn != nullptr) {
    rpc_send_lease_stats(conn);
  }
}

int get_current_target() {
  if (lease_state == lst_off) {
    return rpc_main_target;
  }
  if (lease_state == lst_on) {
    return rpc_lease_target;
  }
  return -1;
}

static int lease_off() {
  assert(lease_state == lst_off);
  if (!lease_ready_flag) {
    return 0;
  }
  if (has_pending_scripts()) {
    return 0;
  }
  if (rpct_ready(rpc_main_target) >= 0) {
    lease_ready_flag = 0;
    return 1;
  }
  return 0;
}

static int lease_on() {
  assert(lease_state == lst_on);
  if (!lease_ready_flag) {
    return 0;
  }
  if (has_pending_scripts()) {
    return 0;
  }
  if (rpct_ready(rpc_lease_target) >= 0) {
    lease_ready_flag = 0;
    return 1;
  }
  return 0;
}

static int lease_start() {
  assert(lease_state == lst_start);
  if (has_pending_scripts()) {
    return 0;
  }
  lease_change_state(lst_on);
  lease_ready_flag = 1;
  if (rpc_stopped) {
    lease_change_state(lst_finish);
  }
  return 1;
}

static int lease_finish() {
  assert(lease_state == lst_finish);
  if (has_pending_scripts()) {
    return 0;
  }
  rpct_stop_ready(rpc_lease_target);
  rpct_lease_stats(rpc_main_target);
  lease_change_state(lst_off);
  lease_ready_flag = 1;
  return 1;
}

void run_rpc_lease() {
  int run_flag = 1;
  while (run_flag) {
    switch (lease_state) {
      case lst_off:
        run_flag = lease_off();
        break;
      case lst_start:
        run_flag = lease_start();
        break;
      case lst_on:
        run_flag = lease_on();
        break;
      case lst_finish:
        run_flag = lease_finish();
        break;
      default:
        assert(0);
    }
  }
}

void lease_cron() {
  int need = 0;

  if (lease_state == lst_on && rpc_lease_timeout < precise_now) {
    lease_change_state(lst_finish);
    need = 1;
  }
  if (lease_ready_flag) {
    need = 1;
  }
  if (need) {
    run_rpc_lease();
  }
}


void do_rpc_stop_lease() {
  if (lease_state != lst_on) {
    return;
  }
  lease_change_state(lst_finish);
  run_rpc_lease();
}

int do_rpc_start_lease(process_id_t pid, double timeout) {
  if (rpc_main_target == -1) {
    return -1;
  }

  if (lease_state != lst_off) {
    return -1;
  }
  int target_fd = get_lease_target_by_pid(pid.ip, pid.port, &rpc_client_ct);
  if (target_fd == -1) {
    return -1;
  }
  if (target_fd == rpc_main_target) {
    vkprintf(0, "can't lease to itself\n");
    return -1;
  }
  if (rpc_stopped) {
    return -1;
  }

  rpc_lease_target = target_fd;
  rpc_lease_timeout = timeout;
  lease_pid = pid;

  lease_stats_cnt = 0;
  lease_stats_start_time = precise_now;
  lease_stats_time = 0;

  lease_change_state(lst_start);
  run_rpc_lease();

  return 0;
}


void lease_set_ready() {
  lease_ready_flag = 1;
}

void lease_on_stop() {
  if (rpc_main_target != -1) {
    conn_target_t *target = &Targets[rpc_main_target];
    connection *conn = get_target_connection(target, 0);
    if (conn != nullptr) {
      rpc_send_stopped(conn);
    }
    do_rpc_stop_lease();
  }
}

void set_main_target(int target) {
  rpc_main_target = target;
}
