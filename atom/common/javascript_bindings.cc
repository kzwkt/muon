// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/common/javascript_bindings.h"

#include <vector>
#include "atom/common/api/api_messages.h"
#include "atom/common/api/atom_api_key_weak_map.h"
#include "atom/common/api/remote_object_freer.h"
#include "atom/common/native_mate_converters/content_converter.h"
#include "atom/common/native_mate_converters/string16_converter.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "base/memory/shared_memory.h"
#include "base/memory/shared_memory_handle.h"
#include "brave/common/extensions/shared_memory_bindings.h"
#include "content/public/renderer/render_frame.h"
#include "extensions/renderer/console.h"
#include "native_mate/dictionary.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_view.h"

using extensions::Feature;

namespace atom {

namespace {

std::vector<v8::Local<v8::Value>> ListValueToVector(v8::Isolate* isolate,
                                                const base::ListValue& list) {
  v8::Local<v8::Value> array = mate::ConvertToV8(isolate, list);
  std::vector<v8::Local<v8::Value>> result;
  mate::ConvertFromV8(isolate, array, &result);
  return result;
}

}  // namespace

JavascriptBindings::JavascriptBindings(content::RenderFrame* render_frame,
                                       extensions::ScriptContext* context)
    : content::RenderFrameObserver(render_frame),
      extensions::ObjectBackedNativeHandler(context) {}

JavascriptBindings::~JavascriptBindings() {}

void JavascriptBindings::AddRoutes() {
  RouteHandlerFunction(
      "GetBinding",
      base::Bind(&JavascriptBindings::GetBinding, base::Unretained(this)));
}

void JavascriptBindings::OnDestruct() {
  // don't self delete on render frame destruction
}

v8::Local<v8::Value> JavascriptBindings::GetHiddenValue(v8::Isolate* isolate,
                                    v8::Local<v8::String> key) {
  if (!is_valid() || !render_frame())
    return v8::Local<v8::Value>();

  v8::Local<v8::Context> v8_context = context()->v8_context();
  v8::Local<v8::Private> privateKey = v8::Private::ForApi(isolate, key);
  v8::Local<v8::Value> value;
  v8::Local<v8::Object> object = v8_context->Global();


  if (!object->HasPrivate(v8_context, privateKey).FromMaybe(false))
    return v8::Local<v8::Value>();

  if (object->GetPrivate(v8_context, privateKey).ToLocal(&value))
    return value;

  return v8::Local<v8::Value>();
}

v8::Local<v8::Value> JavascriptBindings::GetHiddenValueOnObject(
                                    v8::Isolate* isolate,
                                    v8::Local<v8::Object> object,
                                    v8::Local<v8::String> key) {
  v8::Local<v8::Context> v8_context = context()->v8_context();
  v8::Local<v8::Private> privateKey = v8::Private::ForApi(isolate, key);
  v8::Local<v8::Value> value;

  if (!object->HasPrivate(v8_context, privateKey).FromMaybe(false))
    return v8::Local<v8::Value>();

  if (object->GetPrivate(v8_context, privateKey).ToLocal(&value))
    return value;

  return v8::Local<v8::Value>();
}

void JavascriptBindings::SetHiddenValueOnObject(v8::Isolate* isolate,
                    v8::Local<v8::Object> object,
                    v8::Local<v8::String> key,
                    v8::Local<v8::Value> value) {
  if (value.IsEmpty())
    return;
  v8::Local<v8::Context> v8_context = context()->v8_context();
  v8::Local<v8::Private> privateKey = v8::Private::ForApi(isolate, key);
  object->SetPrivate(v8_context, privateKey, value);
}


void JavascriptBindings::SetHiddenValue(v8::Isolate* isolate,
                    v8::Local<v8::String> key,
                    v8::Local<v8::Value> value) {
  if (!is_valid() || !render_frame() || value.IsEmpty())
    return;

  v8::Local<v8::Context> v8_context = context()->v8_context();
  v8::Local<v8::Private> privateKey = v8::Private::ForApi(isolate, key);
  v8_context->Global()->SetPrivate(v8_context, privateKey, value);
}

void JavascriptBindings::DeleteHiddenValue(v8::Isolate* isolate,
                       v8::Local<v8::Object> object,
                       v8::Local<v8::String> key) {
  if (!is_valid() || !render_frame())
    return;

  // Actually deleting the value would force the object into
  // dictionary mode which is unnecessarily slow. Instead, we replace
  // the hidden value with "undefined".
  v8::Local<v8::Context> v8_context = context()->v8_context();
  v8::Local<v8::Private> privateKey = v8::Private::ForApi(isolate, key);
  v8_context->Global()->SetPrivate(
      v8_context, privateKey, v8::Undefined(isolate));
}

void JavascriptBindings::IPCSend(mate::Arguments* args,
          const base::string16& channel,
          const base::ListValue& arguments) {
  if (!is_valid() || !render_frame())
    return;

  bool success = Send(new AtomViewHostMsg_Message(
      routing_id(), channel, arguments));

  if (!success)
    args->ThrowError("Unable to send AtomViewHostMsg_Message");
}

void JavascriptBindings::IPCSendShared(mate::Arguments* args,
            const base::string16& channel,
            base::SharedMemory* shared_memory) {
  if (!is_valid() || !render_frame())
    return;

  base::SharedMemoryHandle memory_handle =
      base::SharedMemory::DuplicateHandle(shared_memory->handle());
  if (!memory_handle.IsValid()) {
    base::SharedMemory::CloseHandle(memory_handle);
    args->ThrowError("Could not create shared memory handle");
    return;
  }

  bool success = Send(new AtomViewHostMsg_Message_Shared(
      routing_id(), channel, memory_handle));

  if (!success)
    args->ThrowError("Unable to send AtomViewHostMsg_Message_Shared");
}

base::string16 JavascriptBindings::IPCSendSync(mate::Arguments* args,
                        const base::string16& channel,
                        const base::ListValue& arguments) {
  base::string16 json;

  if (!is_valid() || !render_frame()) {
    return json;
  }

  IPC::SyncMessage* message = new AtomViewHostMsg_Message_Sync(
      routing_id(), channel, arguments, &json);
  bool success = Send(message);

  if (!success)
    args->ThrowError("Unable to send AtomViewHostMsg_Message_Sync");

  return json;
}

void JavascriptBindings::GetBinding(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
  blink::WebLocalFrame* frame = context()->web_frame();
  DCHECK(frame);

  v8::Isolate* isolate = args.GetIsolate();
  mate::Dictionary binding(isolate, v8::Object::New(isolate));

  mate::Dictionary ipc(isolate, v8::Object::New(isolate));
  ipc.SetMethod("send", base::Bind(&JavascriptBindings::IPCSend,
      base::Unretained(this)));
  ipc.SetMethod("sendSync", base::Bind(&JavascriptBindings::IPCSendSync,
      base::Unretained(this)));
  ipc.SetMethod("sendShared", base::Bind(&JavascriptBindings::IPCSendShared,
      base::Unretained(this)));
  binding.Set("ipc", ipc.GetHandle());

  mate::Dictionary v8(isolate, v8::Object::New(isolate));
  v8.SetMethod("getHiddenValue", base::Bind(&JavascriptBindings::GetHiddenValue,
      base::Unretained(this)));
  v8.SetMethod("setHiddenValue", base::Bind(&JavascriptBindings::SetHiddenValue,
      base::Unretained(this)));
  v8.SetMethod("deleteHiddenValue",
      base::Bind(&JavascriptBindings::DeleteHiddenValue,
      base::Unretained(this)));
  v8.SetMethod("getHiddenValueOnObject",
      base::Bind(&JavascriptBindings::GetHiddenValueOnObject,
      base::Unretained(this)));
  v8.SetMethod("setHiddenValueOnObject",
      base::Bind(&JavascriptBindings::SetHiddenValueOnObject,
      base::Unretained(this)));

  v8.SetMethod("setRemoteObjectFreer", &atom::RemoteObjectFreer::BindTo);
  v8.SetMethod("createIDWeakMap", &atom::api::KeyWeakMap<int32_t>::Create);
  binding.Set("v8", v8.GetHandle());

  args.GetReturnValue().Set(binding.GetHandle());
}

bool JavascriptBindings::OnMessageReceived(const IPC::Message& message) {
  if (!is_valid())
    return false;

  auto context_type = context()->effective_context_type();

  // never handle ipc messages in a web page context
  if (context_type == Feature::WEB_PAGE_CONTEXT)
    return false;

  bool handled = false;

  // Shared memory ipc messages should only be sent to a single context
  // to avoid getting an invalid handle on windows. webui and blessed extension
  // contexts are mutually exclusive
  if (context_type == Feature::WEBUI_CONTEXT ||
      context_type == Feature::BLESSED_EXTENSION_CONTEXT) {
    IPC_BEGIN_MESSAGE_MAP(JavascriptBindings, message)
      IPC_MESSAGE_HANDLER(AtomViewMsg_Message_Shared, OnSharedBrowserMessage)
      IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP()
  }


  IPC_BEGIN_MESSAGE_MAP(JavascriptBindings, message)
    IPC_MESSAGE_HANDLER(AtomViewMsg_Message, OnBrowserMessage)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()

  return handled;
}

void JavascriptBindings::OnSharedBrowserMessage(const base::string16& channel,
                                      const base::SharedMemoryHandle& handle) {
  if (!base::SharedMemory::IsHandleValid(handle)) {
    NOTREACHED() << "Bad handle";
    return;
  }

  if (!is_valid())
    return;

  v8::Isolate* isolate = context()->isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context()->v8_context());

  std::vector<v8::Local<v8::Value>> args_vector;
  args_vector.insert(args_vector.begin(),
      brave::SharedMemoryWrapper::CreateFrom(isolate, handle).ToV8());

  // Insert the Event object, event.sender is ipc
  mate::Dictionary event = mate::Dictionary::CreateEmpty(isolate);
  args_vector.insert(args_vector.begin(), event.GetHandle());

  std::vector<v8::Local<v8::Value>> concatenated_args =
        { mate::StringToV8(isolate, channel) };
      concatenated_args.reserve(1 + args_vector.size());
      concatenated_args.insert(concatenated_args.end(),
                                args_vector.begin(), args_vector.end());

  context()->module_system()->CallModuleMethodSafe("ipc_utils",
                                  "emit",
                                  concatenated_args.size(),
                                  &concatenated_args.front());
}

void JavascriptBindings::OnBrowserMessage(const base::string16& channel,
                                          const base::ListValue& args) {
  if (!context()->is_valid())
    return;

  auto context_type = context()->effective_context_type();
  if (context_type == Feature::WEB_PAGE_CONTEXT)
    return;

  v8::Isolate* isolate = context()->isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context()->v8_context());

  auto args_vector = ListValueToVector(isolate, args);

  // Insert the Event object, event.sender is ipc
  mate::Dictionary event = mate::Dictionary::CreateEmpty(isolate);
  args_vector.insert(args_vector.begin(), event.GetHandle());

  std::vector<v8::Local<v8::Value>> concatenated_args =
        { mate::StringToV8(isolate, channel) };
      concatenated_args.reserve(1 + args_vector.size());
      concatenated_args.insert(concatenated_args.end(),
                                args_vector.begin(), args_vector.end());

  context()->module_system()->CallModuleMethodSafe("ipc_utils",
                                  "emit",
                                  concatenated_args.size(),
                                  &concatenated_args.front());
}

}  // namespace atom
