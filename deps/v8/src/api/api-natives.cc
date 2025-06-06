// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/api/api-natives.h"

#include "src/api/api-inl.h"
#include "src/common/globals.h"
#include "src/common/message-template.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/protectors-inl.h"
#include "src/heap/heap-inl.h"
#include "src/logging/runtime-call-stats-scope.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/lookup.h"
#include "src/objects/templates.h"

namespace v8 {
namespace internal {

namespace {

class V8_NODISCARD InvokeScope {
 public:
  explicit InvokeScope(Isolate* isolate)
      : isolate_(isolate), save_context_(isolate) {}
  ~InvokeScope() {
    bool has_exception = isolate_->has_exception();
    if (has_exception) {
      isolate_->ReportPendingMessages();
    } else {
      isolate_->clear_pending_message();
    }
  }

 private:
  Isolate* isolate_;
  SaveContext save_context_;
};

MaybeHandle<JSObject> InstantiateObject(Isolate* isolate,
                                        DirectHandle<ObjectTemplateInfo> data,
                                        DirectHandle<JSReceiver> new_target,
                                        bool is_prototype);

MaybeHandle<JSFunction> InstantiateFunction(
    Isolate* isolate, DirectHandle<NativeContext> native_context,
    DirectHandle<FunctionTemplateInfo> data,
    MaybeDirectHandle<Name> maybe_name = {});

MaybeHandle<JSFunction> InstantiateFunction(
    Isolate* isolate, DirectHandle<FunctionTemplateInfo> data,
    MaybeDirectHandle<Name> maybe_name = {}) {
  return InstantiateFunction(isolate, isolate->native_context(), data,
                             maybe_name);
}

MaybeDirectHandle<Object> Instantiate(Isolate* isolate,
                                      DirectHandle<Object> data,
                                      MaybeDirectHandle<Name> maybe_name = {}) {
  if (IsFunctionTemplateInfo(*data)) {
    return InstantiateFunction(isolate, Cast<FunctionTemplateInfo>(data),
                               maybe_name);
  } else if (IsObjectTemplateInfo(*data)) {
    return InstantiateObject(isolate, Cast<ObjectTemplateInfo>(data), {},
                             false);
  } else {
    return data;
  }
}

MaybeDirectHandle<Object> DefineAccessorProperty(
    Isolate* isolate, DirectHandle<JSObject> object, DirectHandle<Name> name,
    DirectHandle<Object> getter, DirectHandle<Object> setter,
    PropertyAttributes attributes) {
  DCHECK_IMPLIES(IsFunctionTemplateInfo(*getter),
                 Cast<FunctionTemplateInfo>(*getter)->is_cacheable());
  DCHECK_IMPLIES(IsFunctionTemplateInfo(*setter),
                 Cast<FunctionTemplateInfo>(*setter)->is_cacheable());
  if (IsFunctionTemplateInfo(*getter) &&
      Cast<FunctionTemplateInfo>(*getter)->BreakAtEntry(isolate)) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, getter,
        InstantiateFunction(isolate, Cast<FunctionTemplateInfo>(getter)));
    DirectHandle<Code> trampoline = BUILTIN_CODE(isolate, DebugBreakTrampoline);
    Cast<JSFunction>(getter)->UpdateCode(isolate, *trampoline);
  }
  if (IsFunctionTemplateInfo(*setter) &&
      Cast<FunctionTemplateInfo>(*setter)->BreakAtEntry(isolate)) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, setter,
        InstantiateFunction(isolate, Cast<FunctionTemplateInfo>(setter)));
    DirectHandle<Code> trampoline = BUILTIN_CODE(isolate, DebugBreakTrampoline);
    Cast<JSFunction>(setter)->UpdateCode(isolate, *trampoline);
  }
  RETURN_ON_EXCEPTION(isolate, JSObject::DefineOwnAccessorIgnoreAttributes(
                                   object, name, getter, setter, attributes));
  return object;
}

MaybeDirectHandle<Object> DefineDataProperty(Isolate* isolate,
                                             DirectHandle<JSObject> object,
                                             DirectHandle<Name> name,
                                             DirectHandle<Object> prop_data,
                                             PropertyAttributes attributes) {
  DirectHandle<Object> value;
  ASSIGN_RETURN_ON_EXCEPTION(isolate, value,
                             Instantiate(isolate, prop_data, name));

  PropertyKey key(isolate, name);
  LookupIterator it(isolate, object, key, LookupIterator::OWN_SKIP_INTERCEPTOR);

#ifdef DEBUG
  Maybe<PropertyAttributes> maybe = JSReceiver::GetPropertyAttributes(&it);
  DCHECK(maybe.IsJust());
  if (it.IsFound()) {
    THROW_NEW_ERROR(
        isolate,
        NewTypeError(MessageTemplate::kDuplicateTemplateProperty, name));
  }
#endif

  MAYBE_RETURN_NULL(Object::AddDataProperty(&it, value, attributes,
                                            Just(ShouldThrow::kThrowOnError),
                                            StoreOrigin::kNamed));
  return value;
}

void DisableAccessChecks(Isolate* isolate, DirectHandle<JSObject> object) {
  DirectHandle<Map> old_map(object->map(), isolate);
  // Copy map so it won't interfere constructor's initial map.
  DirectHandle<Map> new_map =
      Map::Copy(isolate, old_map, "DisableAccessChecks");
  new_map->set_is_access_check_needed(false);
  JSObject::MigrateToMap(isolate, object, new_map);
}

void EnableAccessChecks(Isolate* isolate, DirectHandle<JSObject> object) {
  DirectHandle<Map> old_map(object->map(), isolate);
  // Copy map so it won't interfere constructor's initial map.
  DirectHandle<Map> new_map = Map::Copy(isolate, old_map, "EnableAccessChecks");
  new_map->set_is_access_check_needed(true);
  new_map->set_may_have_interesting_properties(true);
  JSObject::MigrateToMap(isolate, object, new_map);
}

class V8_NODISCARD AccessCheckDisableScope {
 public:
  AccessCheckDisableScope(Isolate* isolate, DirectHandle<JSObject> obj)
      : isolate_(isolate),
        disabled_(obj->map()->is_access_check_needed()),
        obj_(obj) {
    if (disabled_) {
      DisableAccessChecks(isolate_, obj_);
    }
  }
  ~AccessCheckDisableScope() {
    if (disabled_) {
      EnableAccessChecks(isolate_, obj_);
    }
  }

 private:
  Isolate* isolate_;
  const bool disabled_;
  DirectHandle<JSObject> obj_;
};

Tagged<Object> GetIntrinsic(Isolate* isolate, v8::Intrinsic intrinsic) {
  DirectHandle<Context> native_context = isolate->native_context();
  DCHECK(!native_context.is_null());
  switch (intrinsic) {
#define GET_INTRINSIC_VALUE(name, iname) \
  case v8::k##name:                      \
    return native_context->iname();
    V8_INTRINSICS_LIST(GET_INTRINSIC_VALUE)
#undef GET_INTRINSIC_VALUE
  }
  return Tagged<Object>();
}

template <typename TemplateInfoT>
MaybeHandle<JSObject> ConfigureInstance(Isolate* isolate, Handle<JSObject> obj,
                                        DirectHandle<TemplateInfoT> data) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kConfigureInstance);
  HandleScope scope(isolate);
  // Disable access checks while instantiating the object.
  AccessCheckDisableScope access_check_scope(isolate, obj);

  // Walk the inheritance chain and copy all accessors to current object.
  int max_number_of_properties = 0;
  Tagged<TemplateInfoT> info = *data;
  while (!info.is_null()) {
    Tagged<Object> props = info->property_accessors();
    if (!IsUndefined(props, isolate)) {
      max_number_of_properties += Cast<ArrayList>(props)->length();
    }
    info = info->GetParent(isolate);
  }

  if (max_number_of_properties > 0) {
    int valid_descriptors = 0;
    // Use a temporary FixedArray to accumulate unique accessors.
    DirectHandle<FixedArray> array =
        isolate->factory()->NewFixedArray(max_number_of_properties);

    // TODO(leszeks): Avoid creating unnecessary handles for cases where we
    // don't need to append anything.
    for (DirectHandle<TemplateInfoT> temp(*data, isolate); !(*temp).is_null();
         temp = direct_handle(temp->GetParent(isolate), isolate)) {
      // Accumulate accessors.
      Tagged<Object> maybe_properties = temp->property_accessors();
      if (!IsUndefined(maybe_properties, isolate)) {
        valid_descriptors = AccessorInfo::AppendUnique(
            isolate, direct_handle(maybe_properties, isolate), array,
            valid_descriptors);
      }
    }

    // Install accumulated accessors.
    for (int i = 0; i < valid_descriptors; i++) {
      DirectHandle<AccessorInfo> accessor(Cast<AccessorInfo>(array->get(i)),
                                          isolate);
      DirectHandle<Name> name(Cast<Name>(accessor->name()), isolate);
      JSObject::SetAccessor(obj, name, accessor,
                            accessor->initial_property_attributes())
          .Assert();
    }
  }

  Tagged<Object> maybe_property_list = data->property_list();
  if (IsUndefined(maybe_property_list, isolate)) return obj;
  DirectHandle<ArrayList> properties(Cast<ArrayList>(maybe_property_list),
                                     isolate);
  if (properties->length() == 0) return obj;

  int i = 0;
  for (int c = 0; c < data->number_of_properties(); c++) {
    auto name = direct_handle(Cast<Name>(properties->get(i++)), isolate);
    Tagged<Object> bit = properties->get(i++);
    if (IsSmi(bit)) {
      PropertyDetails details(Cast<Smi>(bit));
      PropertyAttributes attributes = details.attributes();
      PropertyKind kind = details.kind();

      if (kind == PropertyKind::kData) {
        auto prop_data = handle(properties->get(i++), isolate);
        RETURN_ON_EXCEPTION(isolate, DefineDataProperty(isolate, obj, name,
                                                        prop_data, attributes));
      } else {
        auto getter = direct_handle(properties->get(i++), isolate);
        auto setter = direct_handle(properties->get(i++), isolate);
        RETURN_ON_EXCEPTION(
            isolate, DefineAccessorProperty(isolate, obj, name, getter, setter,
                                            attributes));
      }
    } else {
      // Intrinsic data property --- Get appropriate value from the current
      // context.
      PropertyDetails details(Cast<Smi>(properties->get(i++)));
      PropertyAttributes attributes = details.attributes();
      DCHECK_EQ(PropertyKind::kData, details.kind());

      v8::Intrinsic intrinsic =
          static_cast<v8::Intrinsic>(Smi::ToInt(properties->get(i++)));
      auto prop_data = handle(GetIntrinsic(isolate, intrinsic), isolate);

      RETURN_ON_EXCEPTION(isolate, DefineDataProperty(isolate, obj, name,
                                                      prop_data, attributes));
    }
  }
  return obj;
}

bool IsSimpleInstantiation(Isolate* isolate, Tagged<ObjectTemplateInfo> info,
                           Tagged<JSReceiver> new_target) {
  DisallowGarbageCollection no_gc;

  if (!IsJSFunction(new_target)) return false;
  Tagged<JSFunction> fun = Cast<JSFunction>(new_target);
  if (!fun->shared()->IsApiFunction()) return false;
  if (fun->shared()->api_func_data() != info->constructor()) return false;
  if (info->immutable_proto()) return false;
  return fun->native_context() == isolate->raw_native_context();
}

MaybeHandle<JSObject> InstantiateObject(Isolate* isolate,
                                        DirectHandle<ObjectTemplateInfo> info,
                                        DirectHandle<JSReceiver> new_target,
                                        bool is_prototype) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kInstantiateObject);
  DirectHandle<JSFunction> constructor;
  bool should_cache = info->is_cacheable();
  if (!new_target.is_null()) {
    if (IsSimpleInstantiation(isolate, *info, *new_target)) {
      constructor = Cast<JSFunction>(new_target);
    } else {
      // Disable caching for subclass instantiation.
      should_cache = false;
    }
  }
  // Fast path.
  Handle<JSObject> result;
  if (should_cache) {
    if (TemplateInfo::ProbeInstantiationsCache<JSObject>(
            isolate, isolate->native_context(), info,
            TemplateInfo::CachingMode::kLimited)
            .ToHandle(&result)) {
      return isolate->factory()->CopyJSObject(result);
    }
  }

  if (constructor.is_null()) {
    Tagged<Object> maybe_constructor_info = info->constructor();
    if (IsUndefined(maybe_constructor_info, isolate)) {
      constructor = isolate->object_function();
    } else {
      // Enter a new scope.  Recursion could otherwise create a lot of handles.
      HandleScope scope(isolate);
      DirectHandle<FunctionTemplateInfo> cons_templ(
          Cast<FunctionTemplateInfo>(maybe_constructor_info), isolate);
      DirectHandle<JSFunction> tmp_constructor;
      ASSIGN_RETURN_ON_EXCEPTION(isolate, tmp_constructor,
                                 InstantiateFunction(isolate, cons_templ));
      constructor = scope.CloseAndEscape(tmp_constructor);
    }

    if (new_target.is_null()) new_target = constructor;
  }

  const auto new_js_object_type =
      constructor->has_initial_map() &&
              IsJSApiWrapperObjectMap(constructor->initial_map())
          ? NewJSObjectType::kAPIWrapper
          : NewJSObjectType::kNoAPIWrapper;
  Handle<JSObject> object;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, object,
      JSObject::New(constructor, new_target, {}, new_js_object_type));

  if (is_prototype) JSObject::OptimizeAsPrototype(object);

  ASSIGN_RETURN_ON_EXCEPTION(isolate, result,
                             ConfigureInstance(isolate, object, info));
  if (info->immutable_proto()) {
    JSObject::SetImmutableProto(isolate, object);
  }
  if (!is_prototype) {
    // Keep prototypes in slow-mode. Let them be lazily turned fast later on.
    // TODO(dcarney): is this necessary?
    JSObject::MigrateSlowToFast(result, 0, "ApiNatives::InstantiateObject");
    // Don't cache prototypes.
    if (should_cache) {
      TemplateInfo::CacheTemplateInstantiation(
          isolate, isolate->native_context(), info,
          TemplateInfo::CachingMode::kLimited, result);
      result = isolate->factory()->CopyJSObject(result);
    }
  }

  return result;
}

namespace {
MaybeDirectHandle<Object> GetInstancePrototype(
    Isolate* isolate, DirectHandle<Object> function_template) {
  // Enter a new scope.  Recursion could otherwise create a lot of handles.
  HandleScope scope(isolate);
  DirectHandle<JSFunction> parent_instance;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, parent_instance,
      InstantiateFunction(isolate,
                          Cast<FunctionTemplateInfo>(function_template)));
  Handle<Object> instance_prototype;
  // TODO(cbruni): decide what to do here.
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, instance_prototype,
      JSObject::GetProperty(isolate, parent_instance,
                            isolate->factory()->prototype_string()));
  return scope.CloseAndEscape(instance_prototype);
}
}  // namespace

MaybeHandle<JSFunction> InstantiateFunction(
    Isolate* isolate, DirectHandle<NativeContext> native_context,
    DirectHandle<FunctionTemplateInfo> info,
    MaybeDirectHandle<Name> maybe_name) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kInstantiateFunction);
  bool should_cache = info->is_cacheable();
  if (should_cache) {
    Handle<JSObject> result;
    if (TemplateInfo::ProbeInstantiationsCache<JSObject>(
            isolate, native_context, info,
            TemplateInfo::CachingMode::kUnlimited)
            .ToHandle(&result)) {
      return Cast<JSFunction>(result);
    }
  }
  DirectHandle<Object> prototype;
  if (!info->remove_prototype()) {
    DirectHandle<Object> prototype_templ(info->GetPrototypeTemplate(), isolate);
    if (IsUndefined(*prototype_templ, isolate)) {
      DirectHandle<Object> protoype_provider_templ(
          info->GetPrototypeProviderTemplate(), isolate);
      if (IsUndefined(*protoype_provider_templ, isolate)) {
        prototype = isolate->factory()->NewJSObject(
            direct_handle(native_context->object_function(), isolate));
      } else {
        ASSIGN_RETURN_ON_EXCEPTION(
            isolate, prototype,
            GetInstancePrototype(isolate, protoype_provider_templ));
      }
    } else {
      ASSIGN_RETURN_ON_EXCEPTION(
          isolate, prototype,
          InstantiateObject(isolate, Cast<ObjectTemplateInfo>(prototype_templ),
                            DirectHandle<JSReceiver>(), true));
    }
    DirectHandle<Object> parent(info->GetParentTemplate(), isolate);
    if (!IsUndefined(*parent, isolate)) {
      DirectHandle<Object> parent_prototype;
      ASSIGN_RETURN_ON_EXCEPTION(isolate, parent_prototype,
                                 GetInstancePrototype(isolate, parent));
      DirectHandle<JSPrototype> checked_parent_prototype;
      CHECK(TryCast(parent_prototype, &checked_parent_prototype));
      JSObject::ForceSetPrototype(isolate, Cast<JSObject>(prototype),
                                  checked_parent_prototype);
    }
  }
  InstanceType function_type = JS_SPECIAL_API_OBJECT_TYPE;
  if (!info->needs_access_check() &&
      IsUndefined(info->GetNamedPropertyHandler(), isolate) &&
      IsUndefined(info->GetIndexedPropertyHandler(), isolate)) {
    function_type = v8_flags.experimental_embedder_instance_types
                        ? info->GetInstanceType()
                        : JS_API_OBJECT_TYPE;
    DCHECK(InstanceTypeChecker::IsJSApiObject(function_type));
  }

  Handle<JSFunction> function = ApiNatives::CreateApiFunction(
      isolate, native_context, info, prototype, function_type, maybe_name);
  if (should_cache) {
    // Cache the function.
    TemplateInfo::CacheTemplateInstantiation(
        isolate, native_context, info, TemplateInfo::CachingMode::kUnlimited,
        function);
  }
  MaybeDirectHandle<JSObject> result =
      ConfigureInstance(isolate, function, info);
  if (result.is_null()) {
    // Uncache on error.
    TemplateInfo::UncacheTemplateInstantiation(
        isolate, native_context, info, TemplateInfo::CachingMode::kUnlimited);
    return {};
  }
  info->set_published(true);
  return function;
}

void AddPropertyToPropertyList(Isolate* isolate,
                               DirectHandle<TemplateInfoWithProperties> info,
                               base::Vector<DirectHandle<Object>> data) {
  Tagged<Object> maybe_list = info->property_list();
  DirectHandle<ArrayList> list;
  if (IsUndefined(maybe_list, isolate)) {
    list = ArrayList::New(isolate, static_cast<int>(data.size()),
                          AllocationType::kOld);
  } else {
    list = direct_handle(Cast<ArrayList>(maybe_list), isolate);
  }
  info->set_number_of_properties(info->number_of_properties() + 1);
  for (DirectHandle<Object> value : data) {
    if (value.is_null())
      value = Cast<Object>(isolate->factory()->undefined_value());
    list = ArrayList::Add(isolate, list, value);
  }
  info->set_property_list(*list);
}

}  // namespace

// static
DirectHandle<FunctionTemplateInfo>
ApiNatives::CreateAccessorFunctionTemplateInfo(
    Isolate* i_isolate, FunctionCallback callback, int length,
    SideEffectType side_effect_type) {
  // TODO(v8:5962): move FunctionTemplateNew() from api.cc here.
  auto isolate = reinterpret_cast<v8::Isolate*>(i_isolate);
  Local<FunctionTemplate> func_template = FunctionTemplate::New(
      isolate, callback, v8::Local<Value>{}, v8::Local<v8::Signature>{}, length,
      v8::ConstructorBehavior::kThrow, side_effect_type);
  return Utils::OpenDirectHandle(*func_template);
}

MaybeHandle<JSFunction> ApiNatives::InstantiateFunction(
    Isolate* isolate, DirectHandle<NativeContext> native_context,
    DirectHandle<FunctionTemplateInfo> data,
    MaybeDirectHandle<Name> maybe_name) {
  InvokeScope invoke_scope(isolate);
  return ::v8::internal::InstantiateFunction(isolate, native_context, data,
                                             maybe_name);
}

MaybeHandle<JSFunction> ApiNatives::InstantiateFunction(
    Isolate* isolate, DirectHandle<FunctionTemplateInfo> data,
    MaybeDirectHandle<Name> maybe_name) {
  InvokeScope invoke_scope(isolate);
  return ::v8::internal::InstantiateFunction(isolate, data, maybe_name);
}

MaybeHandle<JSObject> ApiNatives::InstantiateObject(
    Isolate* isolate, DirectHandle<ObjectTemplateInfo> data,
    DirectHandle<JSReceiver> new_target) {
  InvokeScope invoke_scope(isolate);
  return ::v8::internal::InstantiateObject(isolate, data, new_target, false);
}

MaybeHandle<JSObject> ApiNatives::InstantiateRemoteObject(
    DirectHandle<ObjectTemplateInfo> data) {
  Isolate* isolate = data->GetIsolate();
  InvokeScope invoke_scope(isolate);

  DirectHandle<FunctionTemplateInfo> constructor(
      Cast<FunctionTemplateInfo>(data->constructor()), isolate);
  DirectHandle<Map> object_map = isolate->factory()->NewContextlessMap(
      JS_SPECIAL_API_OBJECT_TYPE,
      JSSpecialObject::kHeaderSize +
          data->embedder_field_count() * kEmbedderDataSlotSize,
      TERMINAL_FAST_ELEMENTS_KIND);
  object_map->SetConstructor(*constructor);
  object_map->set_is_access_check_needed(true);
  object_map->set_may_have_interesting_properties(true);

  Handle<JSObject> object = isolate->factory()->NewJSObjectFromMap(
      object_map, AllocationType::kYoung, DirectHandle<AllocationSite>::null(),
      NewJSObjectType::kAPIWrapper);
  JSObject::ForceSetPrototype(isolate, object,
                              isolate->factory()->null_value());

  return object;
}

void ApiNatives::AddDataProperty(Isolate* isolate,
                                 DirectHandle<TemplateInfoWithProperties> info,
                                 DirectHandle<Name> name,
                                 DirectHandle<Object> value,
                                 PropertyAttributes attributes) {
  PropertyDetails details(PropertyKind::kData, attributes,
                          PropertyConstness::kMutable);
  DirectHandle<Object> data[] = {name, direct_handle(details.AsSmi(), isolate),
                                 value};
  AddPropertyToPropertyList(isolate, info, base::VectorOf(data));
}

void ApiNatives::AddDataProperty(Isolate* isolate,
                                 DirectHandle<TemplateInfoWithProperties> info,
                                 DirectHandle<Name> name,
                                 v8::Intrinsic intrinsic,
                                 PropertyAttributes attributes) {
  auto value = direct_handle(Smi::FromInt(intrinsic), isolate);
  auto intrinsic_marker = isolate->factory()->true_value();
  PropertyDetails details(PropertyKind::kData, attributes,
                          PropertyConstness::kMutable);
  DirectHandle<Object> data[] = {
      name, intrinsic_marker, direct_handle(details.AsSmi(), isolate), value};
  AddPropertyToPropertyList(isolate, info, base::VectorOf(data));
}

void ApiNatives::AddAccessorProperty(
    Isolate* isolate, DirectHandle<TemplateInfoWithProperties> info,
    DirectHandle<Name> name, DirectHandle<FunctionTemplateInfo> getter,
    DirectHandle<FunctionTemplateInfo> setter, PropertyAttributes attributes) {
  if (!getter.is_null()) getter->set_published(true);
  if (!setter.is_null()) setter->set_published(true);
  PropertyDetails details(PropertyKind::kAccessor, attributes,
                          PropertyConstness::kMutable);
  DirectHandle<Object> data[] = {name, direct_handle(details.AsSmi(), isolate),
                                 getter, setter};
  AddPropertyToPropertyList(isolate, info, base::VectorOf(data));
}

void ApiNatives::AddNativeDataProperty(
    Isolate* isolate, DirectHandle<TemplateInfoWithProperties> info,
    DirectHandle<AccessorInfo> property) {
  Tagged<Object> maybe_list = info->property_accessors();
  DirectHandle<ArrayList> list;
  if (IsUndefined(maybe_list, isolate)) {
    list = ArrayList::New(isolate, 1, AllocationType::kOld);
  } else {
    list = direct_handle(Cast<ArrayList>(maybe_list), isolate);
  }
  list = ArrayList::Add(isolate, list, property);
  info->set_property_accessors(*list);
}

Handle<JSFunction> ApiNatives::CreateApiFunction(
    Isolate* isolate, DirectHandle<NativeContext> native_context,
    DirectHandle<FunctionTemplateInfo> obj, DirectHandle<Object> prototype,
    InstanceType type, MaybeDirectHandle<Name> maybe_name) {
  RCS_SCOPE(isolate, RuntimeCallCounterId::kCreateApiFunction);
  DirectHandle<SharedFunctionInfo> shared =
      FunctionTemplateInfo::GetOrCreateSharedFunctionInfo(isolate, obj,
                                                          maybe_name);
  // To simplify things, API functions always have shared name.
  DCHECK(shared->HasSharedName());

  Handle<JSFunction> result =
      Factory::JSFunctionBuilder{isolate, shared, native_context}.Build();

  if (obj->remove_prototype()) {
    DCHECK(prototype.is_null());
    DCHECK(result->shared()->IsApiFunction());
    DCHECK(!IsConstructor(*result));
    DCHECK(!result->has_prototype_slot());
    return result;
  }

  // Down from here is only valid for API functions that can be used as a
  // constructor (don't set the "remove prototype" flag).
  DCHECK(result->has_prototype_slot());

  if (obj->read_only_prototype()) {
    result->set_map(isolate,
                    *isolate->sloppy_function_with_readonly_prototype_map());
  }

  if (IsTheHole(*prototype, isolate)) {
    prototype = isolate->factory()->NewFunctionPrototype(result);
  } else if (IsUndefined(obj->GetPrototypeProviderTemplate(), isolate)) {
    JSObject::AddProperty(isolate, Cast<JSObject>(prototype),
                          isolate->factory()->constructor_string(), result,
                          DONT_ENUM);
  }

  int embedder_field_count = 0;
  bool immutable_proto = false;
  if (!IsUndefined(obj->GetInstanceTemplate(), isolate)) {
    DirectHandle<ObjectTemplateInfo> GetInstanceTemplate(
        Cast<ObjectTemplateInfo>(obj->GetInstanceTemplate()), isolate);
    embedder_field_count = GetInstanceTemplate->embedder_field_count();
    immutable_proto = GetInstanceTemplate->immutable_proto();
  }

  // JSFunction requires information about the prototype slot.
  DCHECK(!InstanceTypeChecker::IsJSFunction(type));
  int instance_size = JSObject::GetHeaderSize(type) +
                      kEmbedderDataSlotSize * embedder_field_count;

  DirectHandle<Map> map = isolate->factory()->NewContextfulMap(
      native_context, type, instance_size, TERMINAL_FAST_ELEMENTS_KIND);

  // Mark as undetectable if needed.
  if (obj->undetectable()) {
    // We only allow callable undetectable receivers here, since this whole
    // undetectable business is only to support document.all, which is both
    // undetectable and callable. If we ever see the need to have an object
    // that is undetectable but not callable, we need to update the types.h
    // to allow encoding this.
    CHECK(!IsUndefined(obj->GetInstanceCallHandler(), isolate));

    if (Protectors::IsNoUndetectableObjectsIntact(isolate)) {
      Protectors::InvalidateNoUndetectableObjects(isolate);
    }
    map->set_is_undetectable(true);
  }

  // Mark as needs_access_check if needed.
  if (obj->needs_access_check()) {
    map->set_is_access_check_needed(true);
    map->set_may_have_interesting_properties(true);
  }

  // Set interceptor information in the map.
  if (!IsUndefined(obj->GetNamedPropertyHandler(), isolate)) {
    map->set_has_named_interceptor(true);
    map->set_may_have_interesting_properties(true);
  }
  if (!IsUndefined(obj->GetIndexedPropertyHandler(), isolate)) {
    map->set_has_indexed_interceptor(true);
  }

  // Mark instance as callable in the map.
  if (!IsUndefined(obj->GetInstanceCallHandler(), isolate)) {
    map->set_is_callable(true);
    map->set_is_constructor(!obj->undetectable());
  }

  if (immutable_proto) map->set_is_immutable_proto(true);

  JSFunction::SetInitialMap(isolate, result, map, Cast<JSObject>(prototype));
  return result;
}

}  // namespace internal
}  // namespace v8
