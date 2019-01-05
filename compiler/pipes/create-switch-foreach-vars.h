#pragma once

#include "compiler/function-pass.h"

class CreateSwitchForeachVarsPass : public FunctionPassBase {
private:
  VertexPtr process_switch(VertexPtr v);

  VertexPtr process_foreach(VertexPtr v);


public:
  string get_description() {
    return "create switch and foreach vars";
  }

  VertexPtr on_enter_vertex(VertexPtr v, LocalT *local __attribute__((unused)));
};
