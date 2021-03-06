#include <vector>
#include <v8.h>
#include <nan.h>
#include "polyfill.h"

using namespace v8;

#define V8STR(str) NanNew<String>(str)
#define V8SYM(str) NanNew<String>(str)

#define THROW_BAD_ARGS(FAIL_MSG) NanThrowError(FAIL_MSG);
#define THROW_ERROR(FAIL_MSG) NanThrowTypeError(FAIL_MSG);
#define CHECK_N_ARGS(MIN_ARGS) if (args.Length() < MIN_ARGS) THROW_BAD_ARGS("Expected " #MIN_ARGS " arguments")

const PropertyAttribute CONST_PROP = static_cast<PropertyAttribute>(ReadOnly|DontDelete);

class ProtoBuilder{
public:
	typedef void (*InitFn)(Handle<Object> target);
	typedef std::vector<ProtoBuilder*> ProtoList;
	static ProtoList initProto;

	static void initAll(Handle<Object> target){
		for (auto it=initProto.begin() ; it < initProto.end(); it++ ){
			if ((*it)->initFn) (*(*it)->initFn)(target);
		}

		for (auto it=initProto.begin() ; it < initProto.end(); it++ ){
			(*it)->addToModule(target);
		}
	}

	ProtoBuilder(const char *_name, ProtoBuilder::InitFn _initFn=NULL)
		:name(_name), initFn(_initFn){
		ProtoBuilder::initProto.push_back(this);
		NanAssignPersistent(tpl, NanNew<FunctionTemplate>());
		NanNew<FunctionTemplate>(tpl)->InstanceTemplate()->SetInternalFieldCount(1);
	}

	void init(NanFunctionCallback constructor){
		NanNew<FunctionTemplate>(tpl)->SetCallHandler(constructor);
		NanNew<FunctionTemplate>(tpl)->SetClassName(NanNew<String>(name));
	}

	void inherit(ProtoBuilder& other){
		NanNew<FunctionTemplate>(tpl)->Inherit(NanNew<FunctionTemplate>(other.tpl));
	}

	Handle<Object> get(){
		return NanNew<FunctionTemplate>(tpl)->GetFunction();	
	}

	void addToModule(Handle<Object> target){
		target->Set(NanNew<String>(name), get());
	}

	void addMethod(const char* name, NanFunctionCallback fn){
		NODE_SET_PROTOTYPE_METHOD(NanNew<FunctionTemplate>(tpl), name, fn);
	}

	void addStaticMethod(const char* name, NanFunctionCallback fn){
		NODE_SET_METHOD(NanNew<FunctionTemplate>(tpl), name, fn);
	}

	Persistent<FunctionTemplate> tpl;
	const char* const name;
	const InitFn initFn;
};

// a node ObjectWrap that manages a pointer, deleting it when the v8 object is GC'd
template <class T>
struct PointerWrap: public ObjectWrap{
	PointerWrap(T* v, Handle<Object> js): ptr(v) { Wrap(js); }
	~PointerWrap(){delete ptr;}
	T* ptr;
};

template <class T>
class Proto : public ProtoBuilder{
public:
	typedef T wrappedType;

	Proto(const char *_name, ProtoBuilder::InitFn initfn=NULL):
		ProtoBuilder(_name, initfn){}

	Handle<Object> create(Handle<Value> arg1 = NanUndefined(), Handle<Value> arg2 = NanUndefined()){
		Handle<Value> args[2] = {arg1, arg2};
		Handle<Object> o = NanNew<FunctionTemplate>(tpl)->GetFunction()->NewInstance(2, args);
		return o;
	}

	Handle<Value> create(T* v, Handle<Value> arg1 = NanUndefined(), Handle<Value> arg2 = NanUndefined()){
		if (!v) return NanUndefined();
		Handle<Value> args[3] = {EXTERNAL_NEW(v), arg1, arg2};
		Handle<Object> o = NanNew<FunctionTemplate>(tpl)->GetFunction()->NewInstance(3, args);
		return o;
	}

	inline T* unwrap(Handle<Value> handle){
		if (!handle->IsObject()) return NULL;
		Handle<Object> o = Handle<Object>::Cast(handle);
		if (o->FindInstanceInPrototypeChain(NanNew<FunctionTemplate>(tpl)).IsEmpty()) return NULL;
		return ObjectWrap::Unwrap<T>(o);
	}

	inline T* _wrap(Handle<Object> handle, Handle<Value> ext){
		auto p = static_cast<T*>(External::Cast(*ext)->Value());
		p->attach(handle);
		return p;
	}
};

inline static void setConst(Handle<Object> obj, const char* const name, Handle<Value> value){
	obj->ForceSet(NanNew<String>(name), value, CONST_PROP);
}

#define ENTER_CONSTRUCTOR(MIN_ARGS) \
	NanScope();              \
	if (!args.IsConstructCall()) THROW_ERROR("Must be called with `new`!"); \
	CHECK_N_ARGS(MIN_ARGS);

#define ENTER_CONSTRUCTOR_POINTER(PROTO, MIN_ARGS) \
	ENTER_CONSTRUCTOR(MIN_ARGS)                    \
	if (!args.Length() || !args[0]->IsExternal()){ \
		THROW_BAD_ARGS("This type cannot be created directly!"); \
	}                                               \
	auto self = (PROTO)._wrap(args.This(), args[0]); \
	(void) self

#define ENTER_METHOD(PROTO, MIN_ARGS) \
	NanScope();                \
	CHECK_N_ARGS(MIN_ARGS);           \
	auto self = PROTO.unwrap(args.This()); \
	if (self == NULL) { THROW_BAD_ARGS(#PROTO " method called on invalid object") }

#define ENTER_ACCESSOR(PROTO) \
		NanScope();                \
		auto self = PROTO.unwrap(info.Holder());

#define UNWRAP_ARG(PROTO, NAME, ARGNO)     \
	auto NAME = PROTO.unwrap(args[ARGNO]); \
	if (!NAME)                             \
		THROW_BAD_ARGS("Parameter " #NAME " (" #ARGNO ") is of incorrect type");

#define STRING_ARG(NAME, N) \
	if (args.Length() > N){ \
		if (!args[N]->IsString()) \
			THROW_BAD_ARGS("Parameter " #NAME " (" #N ") should be string"); \
		NAME = *String::Utf8Value(args[N]->ToString()); \
	}

#define DOUBLE_ARG(NAME, N) \
	if (!args[N]->IsNumber()) \
		THROW_BAD_ARGS("Parameter " #NAME " (" #N ") should be number"); \
	NAME = args[N]->ToNumber()->Value();

#define INT_ARG(NAME, N) \
	if (!args[N]->IsNumber()) \
		THROW_BAD_ARGS("Parameter " #NAME " (" #N ") should be number"); \
	NAME = args[N]->ToInt32()->Value();

#define BOOL_ARG(NAME, N) \
	NAME = false;    \
	if (args.Length() > N){ \
		if (!args[N]->IsBoolean()) \
			THROW_BAD_ARGS("Parameter " #NAME " (" #N ") should be bool"); \
		NAME = args[N]->ToBoolean()->Value(); \
	}

#define ARRAY_UNWRAP_ARG(PROTO, TP, NAME, ARGNO)                                \
	if (!args[ARGNO]->IsArray())                                             \
		THROW_BAD_ARGS("Parameter " #NAME " (" #ARGNO ") should be array"); \
	std::vector<TP*> NAME;                         \
	Local<Array> NAME##_arg = Local<Array>::Cast(args[ARGNO]);              \
	for (int NAME##_i=0 ; NAME##_i < (int) NAME##_arg->Length() ; ++NAME##_i) {   \
		auto elem = PROTO.unwrap(NAME##_arg->Get(NAME##_i)); \
		if (!elem) THROW_BAD_ARGS("Parameter " #NAME " (" #ARGNO ") contains element of invalid type"); \
		NAME.push_back(elem);                                           \
	} 
