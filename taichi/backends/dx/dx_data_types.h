#pragma once

#include "taichi/lang_util.h"
#include "taichi/ir/snode.h"
#include <string>

TLANG_NAMESPACE_BEGIN

namespace dx {

inline std::string dx_data_type_name(DataType dt) {
  dt.set_is_pointer(false);
  if (dt->is_primitive(PrimitiveTypeID::f32))
    return "float";
  else if (dt->is_primitive(PrimitiveTypeID::f64))
    return "double";
  else if (dt->is_primitive(PrimitiveTypeID::i32))
    return "int";
  else if (dt->is_primitive(PrimitiveTypeID::i64))
    return "int64_t";
  else if (dt->is_primitive(PrimitiveTypeID::u32))
    return "uint";
  else if (dt->is_primitive(PrimitiveTypeID::u64))
    return "uint64_t";
  else {
    TI_ERROR("Type {} not supported.", dt->to_string());
  }
}

inline int dx_get_snode_meta_size(const SNode *snode) {
  if (snode->type == SNodeType::dynamic) {
    return sizeof(int);
  } else {
    return 0;
  }
}

// Copied from opengl_data_address_shifter
inline int dx_data_address_shifter(DataType type) {
  type.set_is_pointer(false);
  auto dtype_size = data_type_size(type);
  if (dtype_size == 4) {
    return 2;
  } else if (dtype_size == 8) {
    return 3;
  } else {
    TI_NOT_IMPLEMENTED
  }
}

}  // namespace dx

TLANG_NAMESPACE_END