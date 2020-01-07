#pragma once

#include <ATen/core/function_schema.h>
#include <c10/util/Metaprogramming.h>
#include <c10/util/flat_hash_map.h>
#include <c10/util/either.h>
#include <c10/core/TensorTypeId.h>
#include <ATen/core/ivalue.h>
#include <ATen/core/boxing/KernelFunction.h>
#include <ATen/core/dispatch/DispatchKeyExtractor.h>

#include <array>
#include <atomic>
#include <iostream>
#include <mutex>
#include <type_traits>
#include <sstream>
#include <unordered_map>
#include <functional>

namespace c10 {

namespace impl {
/**
 * A KernelFunctionTable is a map from TensorTypeId to a KernelFunction.
 * It can store zero or one KernelFunctions for each TensorTypeId.
 */
class KernelFunctionTable final {
public:
  explicit KernelFunctionTable()
  : kernels_()
  , kernelCount_(0) {}

  enum class SetKernelResult : uint8_t {ADDED_NEW_KERNEL, OVERWROTE_EXISTING_KERNEL};
  C10_NODISCARD SetKernelResult setKernel(TensorTypeId dispatchKey, KernelFunction kernel) {
    TORCH_INTERNAL_ASSERT(dispatchKey != TensorTypeId::UndefinedTensorId);
    auto& slot = kernels_[static_cast<uint8_t>(dispatchKey)];
    SetKernelResult result;;
    if (slot.isValid()) {
      result = SetKernelResult::OVERWROTE_EXISTING_KERNEL;
    } else {
      result = SetKernelResult::ADDED_NEW_KERNEL;
      ++kernelCount_;
    }
    slot = std::move(kernel);
    return result;
  }

  enum class RemoveKernelIfExistsResult : uint8_t {REMOVED_KERNEL, KERNEL_DIDNT_EXIST};
  RemoveKernelIfExistsResult removeKernelIfExists(TensorTypeId dispatchKey) {
    auto& slot = kernels_[static_cast<uint8_t>(dispatchKey)];
    if (slot.isValid()) {
      --kernelCount_;
      slot = {};
      return RemoveKernelIfExistsResult::REMOVED_KERNEL;
    } else {
      return RemoveKernelIfExistsResult::KERNEL_DIDNT_EXIST;
    }
  }

  const KernelFunction& operator[](TensorTypeId dispatchKey) const {
    return kernels_[static_cast<uint8_t>(dispatchKey)];
  }

  KernelFunction& operator[](TensorTypeId dispatchKey) {
    return kernels_[static_cast<uint8_t>(dispatchKey)];
  }

  size_t size() const {
    return kernelCount_;
  }

private:
  std::array<KernelFunction, static_cast<uint8_t>(TensorTypeId::NumTensorIds)> kernels_;
  size_t kernelCount_;
};
}

/**
 * Per-operator dispatch table.
 *
 * Given an operator specified by a FunctionSchema, this class records a dispatch
 * table for various kernels provided for this operator.  For example, if we
 * consider the operator add(Tensor, Tensor), the dispatch table for this
 * operator may contain implementations for various dynamic tensor types, such
 * as CPUTensorId, CUDATensorId, etc.
 */
class DispatchTable final {
 public:
  explicit DispatchTable(const FunctionSchema& schema)
  : kernels_()
  , catchallKernel_()
  , dispatchKeyExtractor_(DispatchKeyExtractor::make(schema))
  , operatorName_(toString(schema.operator_name())) {}

  /**
   * Register a kernel in the table at some dispatch key.
   * @param dispatch_key Dispatch key to define when this kernel is selected.
   * @param kernel Concrete kernel function implementation to register
   */
  void setKernel(TensorTypeId dispatchKey, KernelFunction kernel) {
    if (manuallyBoxedKernel_.has_value()) {
      kernel.setManuallyBoxedKernel_(*manuallyBoxedKernel_);
    }
    auto result = kernels_.setKernel(dispatchKey, std::move(kernel));
    if (result == impl::KernelFunctionTable::SetKernelResult::OVERWROTE_EXISTING_KERNEL) {
      TORCH_WARN("Registered a kernel for operator ", operatorName_, " with dispatch key ", toString(dispatchKey), " that overwrote a previously registered kernel with the same dispatch key for the same operator.");
    }
  }

  /**
   * Deregister the kernel for some dispatch key.
   *
   * @param dispatch_key Dispatch key to unregister.
   */
  void removeKernelIfExists(TensorTypeId dispatchKey) {
    kernels_.removeKernelIfExists(dispatchKey);
  }

  /**
   * Register a catch-all kernel that is called for this operator
   * independent of the inputs. An operator can have either
   * a catch-all kernel or a set of kernels with concrete
   * dispatch keys, not both.
   */
  void setCatchallKernel(KernelFunction kernel) {
    if (catchallKernel_.isValid()) {
      TORCH_WARN("Registered a catch-all kernel for operator ", operatorName_," that overwrote a previously registered catch-all kernel for the same operator.");
    }
    if (manuallyBoxedKernel_.has_value()) {
      kernel.setManuallyBoxedKernel_(*manuallyBoxedKernel_);
    }
    catchallKernel_ = std::move(kernel);
  }

  /**
   * Remove the catch-all kernel.
   */
  void removeCatchallKernel() {
    TORCH_INTERNAL_ASSERT(catchallKernel_.isValid(), "Tried to remove the catch-all kernel for operator ", operatorName_," but there is no catch-all kernel registered.");
    catchallKernel_ = {};
  }

  bool isEmpty() const {
    return !catchallKernel_.isValid() && kernels_.size() == 0;
  }

  std::string listAllDispatchKeys() const {
    std::ostringstream str;
    str << "[";

    bool has_kernels = false;
    for (uint8_t iter = 0; iter != static_cast<uint8_t>(TensorTypeId::NumTensorIds); ++iter) {
      if (!kernels_[static_cast<TensorTypeId>(iter)].isValid()) {
        continue;
      }
      if (has_kernels) {
        str << ", ";
      }
      str << toString(static_cast<TensorTypeId>(iter));
      has_kernels = true;
    }

    if (catchallKernel_.isValid()) {
      if (has_kernels) {
        str << ", ";
      }
      str << "CATCH-ALL";
    }
    str << "]";
    return str.str();
  }

  const KernelFunction* lookup(TensorTypeId dispatchKey) const {
    auto& slot = kernels_[dispatchKey];
    if (slot.isValid()) {
      return &slot;
    } else {
      return nullptr;
    }
  }

  const KernelFunction* lookupCatchallKernel() const {
    if (!catchallKernel_.isValid()) {
      return nullptr;
    }

    return &catchallKernel_;
  }

  const DispatchKeyExtractor& dispatchKeyExtractor() const {
    return dispatchKeyExtractor_;
  }

  const std::string& operatorName() const {
    return operatorName_;
  }

  // This function is a temporary hack, see comment at manuallyBoxedKernel_ member
  void setManuallyBoxedKernel_(KernelFunction::InternalBoxedKernelFunction* func) {
    TORCH_INTERNAL_ASSERT(!manuallyBoxedKernel_.has_value(), "Cannot set multiple manually boxed kernels for the same operator ", operatorName_);

    // make sure that all previously registered kernels get this manually boxed kernel
    for (uint8_t iter = 0; iter != static_cast<uint8_t>(TensorTypeId::NumTensorIds); ++iter) {
      auto& kernel = kernels_[static_cast<TensorTypeId>(iter)];
      if (kernel.isValid()) {
        kernel.setManuallyBoxedKernel_(func);
      }
    }
  }

private:

  impl::KernelFunctionTable kernels_;
  KernelFunction catchallKernel_;
  DispatchKeyExtractor dispatchKeyExtractor_;
  std::string operatorName_;

  // This manuallyBoxedKernel_ member is a temporary hack that allows register_aten_ops.cpp to register its codegen'ed
  // unboxing wrapper for aten operators. We still need those for some operators because not all work
  // with the templated unboxing logic yet.
  // TODO Delete manuallyBoxedKernel_ once all operators work with the templated boxing logic
  c10::optional<KernelFunction::InternalBoxedKernelFunction*> manuallyBoxedKernel_;
};

} // namespace c10
