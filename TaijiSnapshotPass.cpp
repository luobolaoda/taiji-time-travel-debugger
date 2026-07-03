// ================================================================
// 太极快照插桩 Pass — LLVM Module Pass
// 版本: v1.0
// 日期: 2026-07-03
// 编制: 玄同工作室
//
// 在每条 store 指令前插入 snapshot_record() 调用。
// 编译方式:
//   clang++ -shared -fPIC -fno-rtti TaijiSnapshotPass.cpp \
//           $(llvm-config --cxxflags --ldflags --libs) \
//           -o taiji-snapshot.so
//
// 使用方式:
//   clang -fpass-plugin=./taiji-snapshot.so -lsnapshot_runtime \
//         my_program.c -o my_program
// ================================================================

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

// ================================================================
// 配置
// ================================================================
static constexpr bool DEBUG_PASS = false;          // 是否输出调试信息
static constexpr bool SKIP_STACK = true;           // 跳过栈变量的 store
static constexpr bool SKIP_CONSTANT = true;        // 跳过常量地址的 store
static constexpr int  MIN_STORE_SIZE = 1;          // 最小记录的 store 大小
static constexpr int  MAX_STORE_SIZE = 8;          // 最大记录的 store 大小

// ================================================================
// TaijiSnapshotPass — 太极快照插桩 Pass
// ================================================================
struct TaijiSnapshotPass : public PassInfoMixin<TaijiSnapshotPass> {

    // snapshot_record 函数声明（在运行时库中）
    FunctionCallee snapshot_record_func = nullptr;
    // __builtin_return_address(0) 的 intrinsic
    Function* return_address_func = nullptr;

    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
        bool modified = false;

        if (DEBUG_PASS) {
            errs() << "[太极插桩] 开始处理模块: " << M.getName() << "\n";
        }

        // 1. 声明 snapshot_record 函数
        setup_runtime_functions(M);

        // 2. 遍历所有函数中的所有 store 指令
        for (Function &F : M) {
            if (F.isDeclaration()) continue;
            // 跳过运行时库自身
            if (F.getName().startswith("snapshot_")) continue;
            if (F.getName().startswith("crash_")) continue;

            modified |= instrument_function(F);
        }

        if (DEBUG_PASS) {
            errs() << "[太极插桩] 处理完成，修改=" << (modified ? "是" : "否") << "\n";
        }

        return modified ? PreservedAnalyses::none()
                        : PreservedAnalyses::all();
    }

private:
    // ----------------------------------------------------------
    // 设置运行时库函数声明
    // ----------------------------------------------------------
    void setup_runtime_functions(Module &M) {
        LLVMContext &Ctx = M.getContext();

        // void snapshot_record(void* addr, uint64_t old_value, uint64_t pc, int size)
        FunctionType *snap_ty = FunctionType::get(
            Type::getVoidTy(Ctx),
            {
                PointerType::getUnqual(Ctx),     // void* addr
                Type::getInt64Ty(Ctx),           // uint64_t old_value
                Type::getInt64Ty(Ctx),           // uint64_t pc
                Type::getInt32Ty(Ctx)            // int size
            },
            false
        );
        snapshot_record_func = M.getOrInsertFunction("snapshot_record", snap_ty);

        // __builtin_return_address(0)
        return_address_func = Intrinsic::getDeclaration(
            &M, Intrinsic::returnaddress,
            { Type::getInt32Ty(Ctx) }
        );
    }

    // ----------------------------------------------------------
    // 插桩单个函数
    // ----------------------------------------------------------
    bool instrument_function(Function &F) {
        bool modified = false;
        std::vector<StoreInst*> stores;

        // 收集所有 store 指令（不能在遍历时修改）
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    stores.push_back(SI);
                }
            }
        }

        if (stores.empty()) return false;

        LLVMContext &Ctx = F.getContext();
        int instrumented = 0;

        for (StoreInst *SI : stores) {
            if (should_skip(SI)) continue;

            IRBuilder<> Builder(SI);  // 在 store 之前插入

            // 1. 计算写入大小
            Type *val_ty = SI->getValueOperand()->getType();
            int store_size = get_store_size(val_ty);
            if (store_size < MIN_STORE_SIZE || store_size > MAX_STORE_SIZE) continue;

            // 2. 获取被写入的地址
            Value *addr = SI->getPointerOperand();

            // 3. 读取旧值: old = load addr
            Value *old_val = nullptr;
            if (val_ty->isIntegerTy()) {
                // 整数类型：直接 load
                Type *load_ty = Type::getIntNTy(Ctx, store_size * 8);
                old_val = Builder.CreateLoad(load_ty, addr, "snap_old");
                // ZExt 到 64 位
                if (store_size < 8) {
                    old_val = Builder.CreateZExt(old_val, Type::getInt64Ty(Ctx), "snap_old64");
                }
            } else if (val_ty->isPointerTy()) {
                // 指针类型：load 指针值（即指针本身作为整数的值）
                Type *load_ty = Type::getIntNTy(Ctx, store_size * 8);
                old_val = Builder.CreateLoad(load_ty, addr, "snap_old_ptr");
                // ZExt 到 64 位
                if (store_size < 8) {
                    old_val = Builder.CreateZExt(old_val, Type::getInt64Ty(Ctx), "snap_old64");
                }
            } else {
                // 浮点等复杂类型：用 bitcast + load 8字节
                Type *load_ty = Type::getInt64Ty(Ctx);
                Value *bitcast = Builder.CreateBitCast(addr, PointerType::getUnqual(load_ty));
                old_val = Builder.CreateLoad(load_ty, bitcast, "snap_old");
            }

            // 4. 地址转换为 void*
            Value *void_addr = Builder.CreatePointerCast(addr, PointerType::getUnqual(Ctx));

            // 5. 获取当前 PC（返回地址）
            Value *pc = Builder.CreateCall(
                return_address_func,
                { ConstantInt::get(Type::getInt32Ty(Ctx), 0) },
                "snap_pc"
            );
            Value *pc64 = Builder.CreatePtrToInt(pc, Type::getInt64Ty(Ctx), "snap_pc64");

            // 6. 大小常量
            Value *size_val = ConstantInt::get(Type::getInt32Ty(Ctx), store_size);

            // 7. 调用 snapshot_record(addr, old_val, pc, size)
            Builder.CreateCall(snapshot_record_func, {void_addr, old_val, pc64, size_val});

            modified = true;
            instrumented++;
        }

        if (DEBUG_PASS && instrumented > 0) {
            errs() << "  [太极插桩] " << F.getName() << ": "
                   << instrumented << "/" << stores.size() << " 条 store 已插桩\n";
        }

        return modified;
    }

    // ----------------------------------------------------------
    // 判断是否跳过某条 store
    // ----------------------------------------------------------
    bool should_skip(StoreInst *SI) {
        // 跳过栈变量（alloca）
        if (SKIP_STACK) {
            Value *ptr = SI->getPointerOperand();
            // 穿透 bitcast/gep
            while (true) {
                if (isa<AllocaInst>(ptr)) return true;
                if (auto *BC = dyn_cast<BitCastInst>(ptr)) {
                    ptr = BC->getOperand(0);
                    continue;
                }
                if (auto *GEP = dyn_cast<GetElementPtrInst>(ptr)) {
                    ptr = GEP->getPointerOperand();
                    continue;
                }
                break;
            }
        }

        // 跳过全局常量地址
        if (SKIP_CONSTANT) {
            if (auto *GV = dyn_cast<GlobalVariable>(SI->getPointerOperand())) {
                if (GV->isConstant()) return true;
            }
        }

        return false;
    }

    // ----------------------------------------------------------
    // 获取 store 的写入大小（字节）
    // ----------------------------------------------------------
    int get_store_size(Type *ty) {
        if (ty->isIntegerTy()) {
            return ty->getIntegerBitWidth() / 8;
        }
        if (ty->isPointerTy()) {
            return 8;  // 64位指针
        }
        if (ty->isFloatTy()) return 4;
        if (ty->isDoubleTy()) return 8;
        if (ty->isArrayTy() || ty->isStructTy()) {
            // 复合类型：用 DataLayout 计算
            // 简化：取 8
            return 8;
        }
        return 0;  // 未知类型，跳过
    }
};

// ================================================================
// 旧版 Pass 注册（兼容 opt -load）
// ================================================================
struct TaijiSnapshotLegacyPass : public ModulePass {
    static char ID;
    TaijiSnapshotLegacyPass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
        TaijiSnapshotPass pass;
        ModuleAnalysisManager MAM;
        auto result = pass.run(M, MAM);
        return !result.areAllPreserved();
    }

    StringRef getPassName() const override {
        return "太极快照插桩 Pass";
    }
};

char TaijiSnapshotLegacyPass::ID = 0;

} // anonymous namespace

// ================================================================
// 新版 Pass 插件注册（clang -fpass-plugin=taiji-snapshot.so）
// ================================================================

static RegisterPass<TaijiSnapshotLegacyPass> X(
    "taiji-snapshot",
    "太极快照插桩 Pass — 在每条 store 指令前插入 snapshot_record()",
    false, false
);

PassPluginLibraryInfo getTaijiSnapshotPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "TaijiSnapshot",
        "v1.0",
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) -> bool {
                    if (Name == "taiji-snapshot") {
                        MPM.addPass(TaijiSnapshotPass());
                        return true;
                    }
                    return false;
                }
            );
        }
    };
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return getTaijiSnapshotPluginInfo();
}
