#pragma once

#include "compiler/data/data_ptr.h"
#include "compiler/inferring/node.h"
#include "compiler/inferring/type-data.h"
#include "compiler/inferring/type-inferer.h"

namespace tinf {

tinf::Node *get_tinf_node(VertexPtr vertex);
tinf::Node *get_tinf_node(VarPtr var);
tinf::Node *get_tinf_node(FunctionPtr function, int id);

tinf::TypeInferer *get_inferer();
const TypeData *get_type(VertexPtr vertex);
const TypeData *get_type(VarPtr var);
const TypeData *get_type(FunctionPtr function, int id);

}
