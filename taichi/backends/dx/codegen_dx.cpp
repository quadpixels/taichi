#include "codegen_dx.h"
#include "directx_api.h"
#include "dx_data_types.h"

#include "taichi/ir/ir.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/transforms.h"
#include "taichi/util/line_appender.h"
#include "taichi/util/macros.h"
#include "taichi/ir/frontend.h"

// Copied a bunch of stuff from the OpenGL backend :P
TLANG_NAMESPACE_BEGIN

namespace dx {

int kernel_serial = 0;

class KernelGen : public IRVisitor {
  Kernel* kernel;

 public:
  KernelGen(Kernel* kernel,
    std::string kernel_name,
    StructCompiledResult* struct_compiled
  ) : kernel(kernel),
    struct_compiled_(struct_compiled),
    kernel_name_(kernel_name),
    compiled_program_(std::make_unique<CompiledProgram>(kernel)),
    ps(std::make_unique<ParallelSize>()) {
    printf("dx::KernelGen ctor; kernel #%d\n", kernel_serial);
    kernel_serial++;
  }

  void Run(const SNode* root_snode) {
    kernel->ir->accept(this);
  }

  std::unique_ptr<CompiledProgram> get_compiled_program() {
    return std::move(compiled_program_);
  }

  struct ScopedGridStrideLoop {
    KernelGen* gen;
    std::unique_ptr<ScopedIndent> s;
    ScopedGridStrideLoop(KernelGen* gen, int const_iterations)
      : ScopedGridStrideLoop(gen,
        fmt::format("{}", const_iterations),
        const_iterations) {
    }
    ScopedGridStrideLoop(KernelGen* gen,
      std::string iterations,
      int const_iterations = -1)
      : gen(gen) {

      s = std::make_unique<ScopedIndent>(gen->line_appender_);

      TI_ASSERT(gen->ps);
      if (gen->ps->grid_dim == 0) {
        // if not specified, guess an optimal grid_dim for different situations
        // Refs:
        // https://stackoverflow.com/questions/36374652/compute-shaders-optimal-data-division-on-invocations-threads-and-workgroups
        if (const_iterations > 0) {
          if (gen->used_tls) {
            // const range with TLS reduction
            gen->ps->grid_dim =
              std::max((size_t)const_iterations /
                std::max(gen->ps->block_dim, (size_t)1) / 32,
                (size_t)1);
            gen->ps->block_dim = std::max(gen->ps->block_dim / 4, (size_t)1);
          }
          else {
            // const range
            gen->ps->grid_dim =
              std::max(((size_t)const_iterations + gen->ps->block_dim - 1) /
                gen->ps->block_dim,
                (size_t)1);
          }
        }
        else {
          // dynamic range
          // TODO(archibate): think for a better value for SM utilization:
          gen->ps->grid_dim = 256;
        }
      }


      // No way to obtain thread dimension in kernel in DX it seems
      int num_thds = gen->ps->block_dim * gen->ps->grid_dim;
      gen->emit("int _sid0 = int(DTid.x);");
      gen->emit("for (int _sid = _sid0; _sid < ({}); _sid += {}) {{",
        iterations, num_thds);
    }

    ~ScopedGridStrideLoop() {
      s = nullptr;
      gen->emit("}}");
    }
  };

private:
  std::map<int, std::string> ptr_signats; // data or args
  StructCompiledResult *struct_compiled_;
  Stmt *root_stmt_;
  std::unique_ptr<ParallelSize> ps;
  std::string kernel_name_;
  std::string root_snode_type_name_;
  std::unique_ptr<CompiledProgram> compiled_program_;
  bool used_tls; // Thread Local Storage?
  LineAppender line_appender_, line_appender_header_;
  template <typename... Args>
  void emit(std::string f, Args &&... args) {
    line_appender_.append(std::move(f), std::move(args)...);
  }

  std::string dx_data_type_short_name(DataType dt) {
    // TODO: Detect double support
    return data_type_short_name(dt);
  }

  void visit(Block* stmt) override {
    printf("[Block stmt] contains %d stmts\n",
      int(stmt->statements.size()));
    int i = 0;
    for (auto& s : stmt->statements) {
      printf("%d / %d\n", i + 1, stmt->statements.size());
      i++;
      s->accept(this);
    }
  }

  virtual void visit(Stmt* stmt) override {
    printf("[Stmt] not supported\n");
  }

  void visit(PrintStmt* stmt) override {
    printf("[PrintStmt]\n");
  }

  void visit(RandStmt* stmt) override {
    printf("[RandStmt]\n");
  }

  void visit(LinearizeStmt* stmt) override {
    printf("[LinearizeStmt]\n");
  }

  // ?
  void visit(GetRootStmt* stmt) override {
    printf("[GetRootStmt]\n");
    root_stmt_ = stmt;
    emit("int {} = 0;", stmt->short_name());
  }

  void visit(SNodeLookupStmt* stmt) override {
    Stmt *parent = nullptr;
    std::string parent_type;
    if (stmt->input_snode) {
      parent = stmt->input_snode;
      parent_type = stmt->snode->node_type_name;
    } else {
      TI_ASSERT(root_stmt_ != nullptr);
      parent = root_stmt_;
      parent_type = root_snode_type_name_;
    }
    printf("[SNodeLookupStmt] parent_type=%s\n",
      parent_type.c_str());

    TI_ASSERT(parent != nullptr);

    emit("int {} = {} + {} * {}; // {}", stmt->short_name(),
         parent->short_name(),
         struct_compiled_->snode_map.at(parent_type).elem_stride,
         stmt->input_index->short_name(), stmt->snode->node_type_name);

    if (stmt->activate) {
      if (stmt->snode->type == SNodeType::dense) {
        // Do nothing
      } else if (stmt->snode->type == SNodeType::dynamic) {
        TI_ASSERT("dynamic snode activation not implemented" && false);
      }
    }
  }

  // <*gen> $367 = getchild[s0root->s1dense] $366
  // int N = M + 0
  void visit(GetChStmt* stmt) override {
    printf("[GetChStmt]\n");
    emit("int {} = {} + {}; // {}", stmt->short_name(),
         stmt->input_ptr->short_name(),
         struct_compiled_->snode_map.at(stmt->input_snode->node_type_name)
             .children_offsets[stmt->chid],
         stmt->output_snode->node_type_name);
    if (stmt->output_snode->is_place())
      ptr_signats[stmt->id] = "data";
  }

  void visit(GlobalStoreStmt* stmt) override {
    printf("[GlobalStoreStmt]\n");
    TI_ASSERT(stmt->width() == 1);
    auto dt = stmt->data->element_type();
    emit("_{}_{}_[{} >> {}] = {};",
         ptr_signats.at(stmt->ptr->id),  // throw out_of_range if not a pointer
         dx_data_type_short_name(dt), stmt->ptr->short_name(),
         dx_data_address_shifter(dt), stmt->data->short_name());
  }

  void visit(GlobalLoadStmt* stmt) override {
    printf("[GlobalLoadStmt]\n");
    TI_ASSERT(stmt->width() == 1);
    auto dt = stmt->element_type();
    emit("{} {} = _{}_{}_[{} >> {}];",
      dx_data_type_name(stmt->element_type()), stmt->short_name(),
      ptr_signats.at(stmt->ptr->id), dx_data_type_short_name(dt),
      stmt->ptr->short_name(), dx_data_address_shifter(dt));
  }

  void visit(ExternalPtrStmt* stmt) override {
    printf("[ExternalPtrStmt]\n");
  }

  void visit(UnaryOpStmt* stmt) override {
    printf("[UnaryOpStmt]\n");
    std::string dt_name = dx_data_type_name(stmt->element_type());
    switch (stmt->op_type) {
      case UnaryOpType::logic_not: {
        emit("{} {} = {}({} == 0);", dt_name, stmt->short_name(), dt_name,
          stmt->operand->short_name());
        break;
      }
      case UnaryOpType::cast_value: {
        emit("{} {} = {}({});", dt_name, stmt->short_name(),
          dx_data_type_name(stmt->cast_type), stmt->operand->short_name());
        break;
      }
      default: {
        TI_NOT_IMPLEMENTED;
      }
    }
  }

  void visit(BinaryOpStmt* bin) override {
    printf("[BinaryOpStmt]\n");
    const std::string dt_name = dx_data_type_name(bin->element_type());
    const std::string lhs_name = bin->lhs->short_name();
    const std::string rhs_name = bin->rhs->short_name();
    const std::string bin_name = bin->short_name();
    const std::string binop = binary_op_type_symbol(bin->op_type);

    switch (bin->op_type) {
      case BinaryOpType::floordiv: {
        TI_NOT_IMPLEMENTED;
        break;
      }
      case BinaryOpType::mod: {
        TI_NOT_IMPLEMENTED;
        break;
      }
      case BinaryOpType::atan2: {
        TI_NOT_IMPLEMENTED;
        break;
      }
      case BinaryOpType::pow: {
        TI_NOT_IMPLEMENTED;
        break;
      }
      default: {
        emit("{} {} = {} {} {};", dt_name, bin_name, lhs_name, binop, rhs_name);
        break;
      }
    }
  }

  void visit(AtomicOpStmt* stmt) override {
    printf("[AtomicOpStmt]\n");
  }

  void visit(TernaryOpStmt* tri) override {
    printf("[TernaryOpStmt]\n");
  }

  void visit(LocalLoadStmt* stmt) override {
    printf("[LocalLoadStmt]\n");
  }

  void visit(LocalStoreStmt* stmt) override {
    printf("[LocalStoreStmt]\n");
  }

  void visit(AllocaStmt* alloca) override {
    printf("[AllocaStmt]\n");
  }

  // Almost identical to GLSL
  void visit(ConstStmt* const_stmt) override {
    printf("[ConstStmt]\n");
    std::string dt_name = dx_data_type_name(const_stmt->element_type());
    emit("{} {} = {}({});", dt_name, const_stmt->short_name(), dt_name,
         const_stmt->val[0].stringify());
  }

  void visit(KernelReturnStmt* stmt) override {
    printf("[KernelReturnStmt]\n");
    emit("_args_{}_[0] = {};", dx_data_type_short_name(stmt->element_type()),
         stmt->value->short_name());
  }

  // stmt->short_name() <- data
  void visit(ArgLoadStmt* stmt) override {
    printf("[ArgLoadStmt]\n");
    const std::string dt = dx_data_type_name(stmt->element_type());

    if (stmt->is_ptr) {
      TI_ERROR("Pointer is not yet supported");
    } else {
      if (dt == "int" || dt == "float") {
        emit("{} {} = _args_{}32_[{}];", dt, stmt->short_name(), dt[0],
             stmt->arg_id);
      } else {
        TI_ERROR("Data type {} is not yet supported", dt);
      }
    }
  }

  void visit(GlobalTemporaryStmt* stmt) override {
    printf("[GlobalTemporaryStmt]\n");
  }

  void visit(LoopIndexStmt* stmt) override {
    printf("[LoopIndexStmt]\n");
    TI_ASSERT(stmt->index == 0);
    if (stmt->loop->is<OffloadedStmt>()) {
      auto type = stmt->loop->as<OffloadedStmt>()->task_type;
      if (type == OffloadedStmt::TaskType::range_for) {
        emit("int {} = _itv;", stmt->short_name());
      }
      else if (type == OffloadedStmt::TaskType::struct_for) {
        emit("int {} = _itv; // struct for", stmt->short_name());
      }
      else {
        TI_NOT_IMPLEMENTED
      }
    }
    else if (stmt->loop->is<RangeForStmt>()) {
      emit("int {} = {};", stmt->short_name(), stmt->loop->short_name());
    }
    else {
      TI_NOT_IMPLEMENTED;
    }
  }

  void visit(RangeForStmt* for_stmt) override {
    printf("[RangeForStmt]\n");
  }

  void visit(WhileControlStmt* stmt) override {
    printf("[WhileControlStmt]\n");
  }

  void visit(ContinueStmt* stmt) override {
    printf("[ContinueStmt]\n");
  }

  void visit(WhileStmt* stmt) override {
    printf("[WhileStmt]\n");
  }

  void generate_header() {
    emit("RWStructuredBuffer<int> _data_i32_ : register(u0);");
    emit("RWStructuredBuffer<float> _data_f32_ : register(u1);");
    emit("RWStructuredBuffer<int> _args_i32_ : register(u2);");
    emit("RWStructuredBuffer<float> _args_f32_ : register(u3);");
  }

  void generate_bottom() {
    emit("[numthreads({},1,1)]", ps->block_dim);
    emit("void CSMain(uint3 DTid : SV_DispatchThreadID)");
    emit("{{");
    emit("  {}(DTid);", kernel_name_);
    emit("}}");

    std::string kernel_src_code = line_appender_header_.lines() +
      line_appender_.lines();

    printf("kernel_src_code=%s\n", kernel_src_code.c_str());

    compiled_program_->add(kernel_name_, kernel_src_code, std::move(ps));
    line_appender_header_.clear_all();
    line_appender_.clear_all();
    ps = std::make_unique<ParallelSize>();
  }

  void generate_serial_kernel(OffloadedStmt *stmt) {
    TI_ASSERT(stmt->task_type == OffloadedStmt::TaskType::serial);
    const std::string kernel_name = fmt::format("kernel_{}", kernel_serial);
    this->kernel_name_ = kernel_name;
    emit("void {}(uint3 DTid)", kernel_name);
    emit("{{");
    stmt->body->accept(this);
    emit("}}");
  }

  void generate_range_for_kernel(OffloadedStmt* stmt) {
    TI_ASSERT(stmt->task_type == OffloadedStmt::TaskType::range_for);
    const std::string dx_kernel_name = fmt::format("kernel_{}", kernel_serial);
    emit("void {}(uint3 DTid)", dx_kernel_name);
    this->kernel_name_ = dx_kernel_name;
    emit("{{ // range for");

    used_tls = (stmt->tls_prologue != nullptr);
    if (used_tls) {
      TI_NOT_IMPLEMENTED;
      // TLS prologue
    }

    if (stmt->const_begin && stmt->const_end) {
      ScopedIndent _s(line_appender_);
      emit("// Range known at compile time");
      auto begin_value = stmt->begin_value, end_value = stmt->end_value;
      if (end_value < begin_value) end_value = begin_value;
      ps = std::make_unique<ParallelSize>(stmt->block_dim, stmt->grid_dim);
      ScopedGridStrideLoop _gsl(this, end_value - begin_value);
      emit("int _itv = {} + _sid;", begin_value);
      stmt->body->accept(this);
    }
    else {
      TI_NOT_IMPLEMENTED;
    }

    if (used_tls) {
      TI_NOT_IMPLEMENTED;
      // TLS epilogue
    }
    used_tls = false;

    emit("}}\n");
  }

  void visit(OffloadedStmt* stmt) override {

    generate_header();

    emit("//OffloadedStmt");
    TI_TRACE("[OffloadedStmt] raw_name={}, task_name={}",
      stmt->raw_name(), stmt->task_name());


    using Type = OffloadedStmt::TaskType;
    switch (stmt->task_type) {
      case Type::serial:
        printf("Should generate serial\n");
        generate_serial_kernel(stmt);
        break;
      case Type::range_for:
        printf("Should generate range for\n");
        generate_range_for_kernel(stmt);
        break;
      case Type::struct_for:
        printf("Should generate struct for\n");
        break;
      case Type::listgen:
        printf("Should generate listgen\n");
        break;
      default:
        TI_ERROR("[dx] Unsupported offload type={} on dx arch",
                 stmt->task_name());
    }

    generate_bottom();
  }

  void visit(StructForStmt*) override {
    printf("[StructForStmt]\n");
  }

  void visit(IfStmt* if_stmt) override {
    printf("[IfStmt]\n");
    emit("if ({} != 0) {{", if_stmt->cond->short_name());
    if (if_stmt->true_statements) {
      if_stmt->true_statements->accept(this);
    }
    if (if_stmt->false_statements) {
      emit("}} else {{");
      if_stmt->false_statements->accept(this);
    }
    emit("}}");
  }

  void visit(ElementShuffleStmt* es_stmt) override {
    printf("[ElementShuffleStmt]\n");
  }
};

void DummyFunc(Context& ctx) {
  printf("[dx::DummyFunc]\n");
}

FunctionType DxCodeGen::Compile(Program* program, Kernel* kernel) {
  {
    printf("[compile] Lowering the IR\n");
    auto ir = kernel->ir.get();
    auto& config = kernel->program.config;
    config.demote_dense_struct_fors = true;
    irpass::compile_to_executable(ir, config,
        false, kernel->grad,
        false, config.print_ir,
        true,
        config.make_thread_local);
    irpass::print(ir);
  }

  KernelGen kg(kernel, kernel->name, this->struct_compiled_);
  kg.Run(program->snode_root.get());

  std::unique_ptr<CompiledProgram> compiled =
      std::move(kg.get_compiled_program());
  taichi::lang::dx::CompiledProgram *ptr = compiled.get();

  // Pass the ownership of the std::unique_ptr to the kernel launcher
  kernel_launcher_->keep(std::move(compiled));

  return [ptr, launcher = kernel_launcher_](Context& ctx) { 
    printf("[DxCodeGen] We should launch a DX kernel now %p\n", &ctx);
    ptr->launch(ctx, launcher);
  };
}

}

TLANG_NAMESPACE_END