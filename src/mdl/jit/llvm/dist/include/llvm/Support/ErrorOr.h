//===- llvm/Support/ErrorOr.h - Error Smart Pointer -----------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Provides ErrorOr<T> smart pointer.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_ERROR_OR_H
#define LLVM_SUPPORT_ERROR_OR_H

#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/type_traits.h"

#include <cassert>
#if LLVM_HAS_CXX11_TYPETRAITS
#include <type_traits>
#endif

namespace llvm {
#if LLVM_HAS_CXX11_TYPETRAITS && LLVM_HAS_RVALUE_REFERENCES
template<class T, class V>
typename MISTD::enable_if< MISTD::is_constructible<T, V>::value
                       , typename MISTD::remove_reference<V>::type>::type &&
 moveIfMoveConstructible(V &Val) {
  return MISTD::move(Val);
}

template<class T, class V>
typename MISTD::enable_if< !MISTD::is_constructible<T, V>::value
                       , typename MISTD::remove_reference<V>::type>::type &
moveIfMoveConstructible(V &Val) {
  return Val;
}
#else
template<class T, class V>
V &moveIfMoveConstructible(V &Val) {
  return Val;
}
#endif

/// \brief Stores a reference that can be changed.
template <typename T>
class ReferenceStorage {
  T *Storage;

public:
  ReferenceStorage(T &Ref) : Storage(&Ref) {}

  operator T &() const { return *Storage; }
  T &get() const { return *Storage; }
};

/// \brief Represents either an error or a value T.
///
/// ErrorOr<T> is a pointer-like class that represents the result of an
/// operation. The result is either an error, or a value of type T. This is
/// designed to emulate the usage of returning a pointer where nullptr indicates
/// failure. However instead of just knowing that the operation failed, we also
/// have an error_code and optional user data that describes why it failed.
///
/// It is used like the following.
/// \code
///   ErrorOr<Buffer> getBuffer();
///   void handleError(error_code ec);
///
///   auto buffer = getBuffer();
///   if (!buffer)
///     handleError(buffer);
///   buffer->write("adena");
/// \endcode
///
///
/// An implicit conversion to bool provides a way to check if there was an
/// error. The unary * and -> operators provide pointer like access to the
/// value. Accessing the value when there is an error has undefined behavior.
///
/// When T is a reference type the behaivor is slightly different. The reference
/// is held in a MISTD::reference_wrapper<MISTD::remove_reference<T>::type>, and
/// there is special handling to make operator -> work as if T was not a
/// reference.
///
/// T cannot be a rvalue reference.
template<class T>
class ErrorOr {
  template <class OtherT> friend class ErrorOr;
  static const bool isRef = is_reference<T>::value;
  typedef ReferenceStorage<typename remove_reference<T>::type> wrap;

public:
  typedef typename
    conditional< isRef
               , wrap
               , T
               >::type storage_type;

private:
  typedef typename remove_reference<T>::type &reference;
  typedef typename remove_reference<T>::type *pointer;

public:
  template <class E>
  ErrorOr(E ErrorCode, typename enable_if_c<is_error_code_enum<E>::value ||
                                            is_error_condition_enum<E>::value,
                                            void *>::type = 0)
      : HasError(true) {
    new (getError()) error_code(make_error_code(ErrorCode));
  }

  ErrorOr(llvm::error_code EC) : HasError(true) {
    new (getError()) error_code(EC);
  }

  ErrorOr(T Val) : HasError(false) {
    new (get()) storage_type(moveIfMoveConstructible<storage_type>(Val));
  }

  ErrorOr(const ErrorOr &Other) {
    copyConstruct(Other);
  }

  template <class OtherT>
  ErrorOr(const ErrorOr<OtherT> &Other) {
    copyConstruct(Other);
  }

  ErrorOr &operator =(const ErrorOr &Other) {
    copyAssign(Other);
    return *this;
  }

  template <class OtherT>
  ErrorOr &operator =(const ErrorOr<OtherT> &Other) {
    copyAssign(Other);
    return *this;
  }

#if LLVM_HAS_RVALUE_REFERENCES
  ErrorOr(ErrorOr &&Other) {
    moveConstruct(MISTD::move(Other));
  }

  template <class OtherT>
  ErrorOr(ErrorOr<OtherT> &&Other) {
    moveConstruct(MISTD::move(Other));
  }

  ErrorOr &operator =(ErrorOr &&Other) {
    moveAssign(MISTD::move(Other));
    return *this;
  }

  template <class OtherT>
  ErrorOr &operator =(ErrorOr<OtherT> &&Other) {
    moveAssign(MISTD::move(Other));
    return *this;
  }
#endif

  ~ErrorOr() {
    if (!HasError)
      get()->~storage_type();
  }

  typedef void (*unspecified_bool_type)();
  static void unspecified_bool_true() {}

  /// \brief Return false if there is an error.
  operator unspecified_bool_type() const {
    return HasError ? 0 : unspecified_bool_true;
  }

  operator llvm::error_code() const {
    return HasError ? *getError() : llvm::error_code::success();
  }

  pointer operator ->() {
    return toPointer(get());
  }

  reference operator *() {
    return *get();
  }

private:
  template <class OtherT>
  void copyConstruct(const ErrorOr<OtherT> &Other) {
    if (!Other.HasError) {
      // Get the other value.
      HasError = false;
      new (get()) storage_type(*Other.get());
    } else {
      // Get other's error.
      HasError = true;
      new (getError()) error_code(Other);
    }
  }

  template <class T1>
  static bool compareThisIfSameType(const T1 &a, const T1 &b) {
    return &a == &b;
  }

  template <class T1, class T2>
  static bool compareThisIfSameType(const T1 &a, const T2 &b) {
    return false;
  }

  template <class OtherT>
  void copyAssign(const ErrorOr<OtherT> &Other) {
    if (compareThisIfSameType(*this, Other))
      return;

    this->~ErrorOr();
    new (this) ErrorOr(Other);
  }

#if LLVM_HAS_RVALUE_REFERENCES
  template <class OtherT>
  void moveConstruct(ErrorOr<OtherT> &&Other) {
    if (!Other.HasError) {
      // Get the other value.
      HasError = false;
      new (get()) storage_type(MISTD::move(*Other.get()));
    } else {
      // Get other's error.
      HasError = true;
      new (getError()) error_code(Other);
    }
  }

  template <class OtherT>
  void moveAssign(ErrorOr<OtherT> &&Other) {
    if (compareThisIfSameType(*this, Other))
      return;

    this->~ErrorOr();
    new (this) ErrorOr(MISTD::move(Other));
  }
#endif

  pointer toPointer(pointer Val) {
    return Val;
  }

  pointer toPointer(wrap *Val) {
    return &Val->get();
  }

  storage_type *get() {
    assert(!HasError && "Cannot get value when an error exists!");
    return reinterpret_cast<storage_type*>(TStorage.buffer);
  }

  const storage_type *get() const {
    assert(!HasError && "Cannot get value when an error exists!");
    return reinterpret_cast<const storage_type*>(TStorage.buffer);
  }

  error_code *getError() {
    assert(HasError && "Cannot get error when a value exists!");
    return reinterpret_cast<error_code*>(ErrorStorage.buffer);
  }

  const error_code *getError() const {
    return const_cast<ErrorOr<T> *>(this)->getError();
  }


  union {
    AlignedCharArrayUnion<storage_type> TStorage;
    AlignedCharArrayUnion<error_code> ErrorStorage;
  };
  bool HasError : 1;
};

template<class T, class E>
typename enable_if_c<is_error_code_enum<E>::value ||
                     is_error_condition_enum<E>::value, bool>::type
operator ==(ErrorOr<T> &Err, E Code) {
  return error_code(Err) == Code;
}
} // end namespace llvm

#endif
