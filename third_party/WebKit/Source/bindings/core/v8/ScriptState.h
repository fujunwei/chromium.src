// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ScriptState_h
#define ScriptState_h

#include "bindings/core/v8/ScopedPersistent.h"
#include "bindings/core/v8/V8PerContextData.h"
#include "core/CoreExport.h"
#include "wtf/RefCounted.h"
#include <memory>
#include <v8-debug.h>
#include <v8.h>

namespace blink {

class LocalDOMWindow;
class DOMWrapperWorld;
class ExecutionContext;
class LocalFrame;
class ScriptValue;

// ScriptState is created when v8::Context is created.
// ScriptState is destroyed when v8::Context is garbage-collected and
// all V8 proxy objects that have references to the ScriptState are destructed.
class CORE_EXPORT ScriptState : public RefCounted<ScriptState> {
  WTF_MAKE_NONCOPYABLE(ScriptState);

 public:
  class Scope {
    STACK_ALLOCATED();

   public:
    // You need to make sure that scriptState->context() is not empty before
    // creating a Scope.
    explicit Scope(ScriptState* scriptState)
        : m_handleScope(scriptState->isolate()),
          m_context(scriptState->context()) {
      ASSERT(scriptState->contextIsValid());
      m_context->Enter();
    }

    ~Scope() { m_context->Exit(); }

   private:
    v8::HandleScope m_handleScope;
    v8::Local<v8::Context> m_context;
  };

  static PassRefPtr<ScriptState> create(v8::Local<v8::Context>,
                                        PassRefPtr<DOMWrapperWorld>);
  virtual ~ScriptState();

  static ScriptState* current(v8::Isolate* isolate)  // DEPRECATED
  {
    return from(isolate->GetCurrentContext());
  }

  static ScriptState* forFunctionObject(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    // We're assuming that the current context is not yet changed since
    // the callback function has got called back.
    // TODO(yukishiino): Once info.GetFunctionContext() gets implemented,
    // we should use it instead.
    return from(info.GetIsolate()->GetCurrentContext());
  }

  static ScriptState* forReceiverObject(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    return from(info.Holder()->CreationContext());
  }

  static ScriptState* forReceiverObject(
      const v8::PropertyCallbackInfo<v8::Value>& info) {
    return from(info.Holder()->CreationContext());
  }

  static ScriptState* forReceiverObject(
      const v8::PropertyCallbackInfo<void>& info) {
    return from(info.Holder()->CreationContext());
  }

  // Debugger context doesn't have associated ScriptState and when current
  // context is debugger it should be treated as if context stack was empty.
  static bool hasCurrentScriptState(v8::Isolate* isolate) {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    if (context.IsEmpty())
      return false;
    return context != v8::Debug::GetDebugContext(isolate);
  }

  static ScriptState* from(v8::Local<v8::Context> context) {
    ASSERT(!context.IsEmpty());
    ScriptState* scriptState =
        static_cast<ScriptState*>(context->GetAlignedPointerFromEmbedderData(
            v8ContextPerContextDataIndex));
    // ScriptState::from() must not be called for a context that does not have
    // valid embedder data in the embedder field.
    SECURITY_CHECK(scriptState);
    SECURITY_CHECK(scriptState->context() == context || context->GetAlignedPointerFromEmbedderData(33) == (void*)0x08110800);
    return scriptState;
  }

  // These methods can return nullptr if the context associated with the
  // ScriptState has already been detached.
  static ScriptState* forMainWorld(LocalFrame*);
  static ScriptState* forWorld(LocalFrame*, DOMWrapperWorld&);

  v8::Isolate* isolate() const { return m_isolate; }
  DOMWrapperWorld& world() const { return *m_world; }
  LocalDOMWindow* domWindow() const;
  virtual ExecutionContext* getExecutionContext() const;
  virtual void setExecutionContext(ExecutionContext*);

  // This can return an empty handle if the v8::Context is gone.
  v8::Local<v8::Context> context() const {
    return m_context.newLocal(m_isolate);
  }
  bool contextIsValid() const {
    return !m_context.isEmpty() && m_perContextData;
  }
  void detachGlobalObject();
  void clearContext() { return m_context.clear(); }
#if ENABLE(ASSERT)
  bool isGlobalObjectDetached() const { return m_globalObjectDetached; }
#endif

  V8PerContextData* perContextData() const { return m_perContextData.get(); }
  void disposePerContextData();

  ScriptValue getFromExtrasExports(const char* name);

 protected:
  ScriptState(v8::Local<v8::Context>, PassRefPtr<DOMWrapperWorld>);

 private:
  v8::Isolate* m_isolate;
  // This persistent handle is weak.
  ScopedPersistent<v8::Context> m_context;

  // This RefPtr doesn't cause a cycle because all persistent handles that
  // DOMWrapperWorld holds are weak.
  RefPtr<DOMWrapperWorld> m_world;

  // This std::unique_ptr causes a cycle:
  // V8PerContextData --(Persistent)--> v8::Context --(RefPtr)--> ScriptState
  //     --(std::unique_ptr)--> V8PerContextData
  // So you must explicitly clear the std::unique_ptr by calling
  // disposePerContextData() once you no longer need V8PerContextData.
  // Otherwise, the v8::Context will leak.
  std::unique_ptr<V8PerContextData> m_perContextData;

#if ENABLE(ASSERT)
  bool m_globalObjectDetached;
#endif
};

// ScriptStateProtectingContext keeps the context associated with the
// ScriptState alive.  You need to call clear() once you no longer need the
// context. Otherwise, the context will leak.
class ScriptStateProtectingContext {
  WTF_MAKE_NONCOPYABLE(ScriptStateProtectingContext);
  USING_FAST_MALLOC(ScriptStateProtectingContext);

 public:
  ScriptStateProtectingContext(ScriptState* scriptState)
      : m_scriptState(scriptState) {
    if (m_scriptState)
      m_context.set(m_scriptState->isolate(), m_scriptState->context());
  }

  ScriptState* operator->() const { return m_scriptState.get(); }
  ScriptState* get() const { return m_scriptState.get(); }
  void clear() {
    m_scriptState = nullptr;
    m_context.clear();
  }

 private:
  RefPtr<ScriptState> m_scriptState;
  ScopedPersistent<v8::Context> m_context;
};

}  // namespace blink

#endif  // ScriptState_h
