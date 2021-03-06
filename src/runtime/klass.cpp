/*
 * klass.cpp
 *
 *  Created on: 2017年11月2日
 *      Author: zhengxiaolin
 */

#include "native/java_lang_Class.hpp"
#include "runtime/klass.hpp"
#include "runtime/field.hpp"
#include "classloader.hpp"
#include "runtime/constantpool.hpp"
#include "runtime/oop.hpp"
#include <utility>
#include <cstring>
#include <sstream>
#include <algorithm>

using std::make_pair;
using std::make_shared;
using std::wstringstream;

/*===----------------   aux   --------------------===*/
Type get_type(const wstring & name)
{
	assert(name.size() == 1);		// B,C,D,J,S,.... only one char.
	Type type;
	switch (name[0]) {
		case L'Z':{
			type = Type::BOOLEAN;
			break;
		}
		case L'B':{
			type = Type::BYTE;
			break;
		}
		case L'C':{
			type = Type::CHAR;
			break;
		}
		case L'S':{
			type = Type::SHORT;
			break;
		}
		case L'I':{
			type = Type::INT;
			break;
		}
		case L'F':{
			type = Type::FLOAT;
			break;
		}
		case L'J':{
			type = Type::LONG;
			break;
		}
		case L'D':{
			type = Type::DOUBLE;
			break;
		}
		default:{
			std::cerr << "can't get here!" << std::endl;
			assert(false);
		}
	}
	return type;
}

Lock & Rt_Pool::rt_pool_lock(){
	static Lock rt_pool_lock;
	return rt_pool_lock;
}
list<rt_constant_pool *> & Rt_Pool::rt_pool() {
	static list<rt_constant_pool *> rt_pool;		// 存放所有的对象，以备日后的 delete。
	return rt_pool;
}
void Rt_Pool::put(rt_constant_pool *pool) {
	LockGuard lg(rt_pool_lock());
	rt_pool().push_back(pool);
}
void Rt_Pool::cleanup() {
	LockGuard lg(rt_pool_lock());
	for (auto iter : rt_pool()) {
		delete iter;
	}
}

/*===----------------  InstanceKlass  ------------------===*/
void InstanceKlass::parse_fields(ClassFile *cf)
{
	// ** first copy parent's non-static fields / interfaces' non-static fields here ** (don't copy static fields !!)
	// 1. super_klass
	if (this->parent != nullptr) {		// if this_klass is **NOT** java.lang.Object
		auto & super_map = ((InstanceKlass *)this->parent)->fields_layout;
#ifdef DEBUG
		sync_wcout{} << "now this klass is ..." << this->get_name() << "... super klass is ..." << this->parent->get_name() << std::endl;
#endif
		for (auto iter : super_map) {		// 注意... unordered_map 乱序存储... 所以也不可能按照顺序来... 想要直接使用 vector<bool> 来指定一个 field 是否是自己的，使用下标来存取是落空了...因为顺序都不定...push_back 肯定是从 1..2..3.. 挨个走，而从 super 那里读取到的 map 却是乱序的 1..4..2.. 不断使用 reserve 或许可以。 我就懒一点，使用 unordered_map <int, bool> 好了... 暴力省事。虽然先计算出来所有的 field 数目才是好主意...
			this->fields_layout[iter.first] = iter.second;
			this->is_this_klass_field.insert(make_pair(this->fields_layout[iter.first].first, false));		// 不是自己的 field
		}
		this->total_non_static_fields_num = ((InstanceKlass *)this->parent)->total_non_static_fields_num;
	}
	// 2. interfaces
	for (auto iter : this->interfaces) {
		auto layout = iter.second->fields_layout;
		for (auto field_iter : layout) {
			this->fields_layout[iter.first] = make_pair(total_non_static_fields_num, field_iter.second.second);
			total_non_static_fields_num ++;
			this->is_this_klass_field.insert(make_pair(this->fields_layout[iter.first].first, false));		// 不是自己的 field
		}
	}
	// 3. this_klass
	wstringstream ss;
	// set up Runtime Field_info to transfer Non-Dynamic field_info
	for (int i = 0; i < cf->fields_count; i ++) {
		Field_info *metaField = new Field_info(this, cf->fields[i], cf->constant_pool);
		Field_Pool::put(metaField);		// put it into global area
		if (!metaField->is_static()) {
			ss << metaField->get_klass()->get_name() << L":";		// for fixing bug: must have the namespace in field_layout... !!!
		}
		ss << metaField->get_name() << L":" << metaField->get_descriptor();
		if(metaField->is_static()) {	// static field
			this->static_fields_layout.insert(make_pair(ss.str(), make_pair(total_static_fields_num, metaField)));
			total_static_fields_num ++;	// offset +++
		} else {		// non-static field
			this->fields_layout.insert(make_pair(ss.str(), make_pair(total_non_static_fields_num, metaField)));
			this->is_this_klass_field.insert(make_pair(total_non_static_fields_num, true));		// 是自己的 field
			total_non_static_fields_num ++;
		}
		ss.str(L"");
	}

	// alloc to save value of STATIC fields. non-statics are in oop.
//	if (total_static_fields_num != 0)		// be careful!!!!
//		this->static_fields = new Oop*[total_static_fields_num];
//	memset(this->static_fields, 0, total_static_fields_num * sizeof(Oop *));	// bzero!!

	this->static_fields.resize(total_static_fields_num, nullptr);		// alloc and bzero!!

	// initialize static BasicTypeOop...
	initialize_field(this->static_fields_layout, this->static_fields);

#ifdef KLASS_DEBUG
	sync_wcout{} << "===--------------- (" << this->get_name() << ") Debug Runtime FieldPool ---------------===" << std::endl;
	sync_wcout{} << "static Field: " << this->static_fields_layout.size() << "; non-static Field: " << this->fields_layout.size() << std::endl;
	if (this->fields_layout.size() != 0)		sync_wcout{} << "non-static as below:" << std::endl;
	int counter = 0;
	for (auto iter : this->fields_layout) {
		sync_wcout{} << "  #" << counter++ << "  name: " << iter.first << ", offset: " << iter.second.first << std::endl;
	}
	counter = 0;
	if (this->static_fields_layout.size() != 0)	sync_wcout{} << "static as below:" << std::endl;
	for (auto iter : this->static_fields_layout) {
		sync_wcout{} << "  #" << counter++ << "  name: " << iter.first << ", offset: " << iter.second.first << std::endl;
	}
	sync_wcout{} << "===--------------------------------------------------------===" << std::endl;
#endif
}

void InstanceKlass::parse_superclass(ClassFile *cf, ClassLoader *loader)
{
	if (cf->super_class == 0) {	// this class = java/lang/Object		// TODO: java.lang.Object 怎么办？是个接口？？
		this->parent = nullptr;
	} else {			// base class
		assert(cf->constant_pool[cf->super_class-1]->tag == CONSTANT_Class);
		wstring super_name = ((CONSTANT_Utf8_info *)cf->constant_pool[((CONSTANT_CS_info *)cf->constant_pool[cf->super_class-1])->index-1])->convert_to_Unicode();
		if (loader == nullptr) {	// bootstrap classloader do this
			this->parent = ((InstanceKlass *)BootStrapClassLoader::get_bootstrap().loadClass(super_name));
		} else {		// my classloader do this
			this->parent = ((InstanceKlass *)loader->loadClass(super_name));
		}

//		if (this->parent != nullptr) {
//			this->next_sibling = this->parent->get_child();		// set this's elder brother	// note: this->parent->child can't pass the compile. because this->parent is okay, but parent->child is visiting Klass in the InstanceKlass. `Protected` is: InstanceKlass can visit [the Klass part] inside of the IntanceKlass object. But here is: InstanceKlass visit the Klass part inside of the InstanceKlass part(this->parent), but then visit the Klass outer class (parent->child). parent variable is inside the InstanceKlass, but point to an outer Klass not in the InstanceKlass. To solve it, only use setters and getters.
//			this->parent->set_child(InstanceKlass *(this, [](InstanceKlass*){}));			// set parent's newest child
//			// above ↑ is a little hack. I don't know whether there is a side effect.
//		}
	}
#ifdef KLASS_DEBUG
	sync_wcout{} << "===--------------- (" << this->get_name() << ") Debug SuperClass ---------------===" << std::endl;
	if (cf->super_class == 0) {
		sync_wcout{} << "this class is **java.lang.Object** class and doesn't have a superclass." << std::endl;
	} else {
		sync_wcout{} << "superclass:  #" << cf->super_class << ", name: " << this->parent->get_name() << std::endl;
	}
	sync_wcout{} << "===-------------------------------------------------===" << std::endl;
#endif
}

void InstanceKlass::parse_interfaces(ClassFile *cf, ClassLoader *loader)	// interface should also be made by the InstanceKlass!!
{
	for(int i = 0; i < cf->interfaces_count; i ++) {
		// get interface name
		assert(cf->constant_pool[cf->interfaces[i]-1]->tag == CONSTANT_Class);
		wstring interface_name = ((CONSTANT_Utf8_info *)cf->constant_pool[((CONSTANT_CS_info *)cf->constant_pool[cf->interfaces[i]-1])->index-1])->convert_to_Unicode();
		InstanceKlass *interface;
		if (loader == nullptr) {
			interface = ((InstanceKlass *)BootStrapClassLoader::get_bootstrap().loadClass(interface_name));
		} else {
			interface = ((InstanceKlass *)loader->loadClass(interface_name));
			assert(interface != nullptr);
		}
		assert(interface != nullptr);
		this->interfaces.insert(make_pair(interface_name, interface));
	}
#ifdef KLASS_DEBUG
	sync_wcout{} << "===--------------- (" << this->get_name() << ") Debug Runtime InterfacePool ---------------===" << std::endl;
	sync_wcout{} << "interfaces: total " << this->interfaces.size() << std::endl;
	int counter = 0;
	for (auto iter : this->interfaces) {
		sync_wcout{} << "  #" << counter++ << "  name: " << iter.first << std::endl;
	}
	sync_wcout{} << "===------------------------------------------------------------===" << std::endl;
#endif
}

void InstanceKlass::parse_methods(ClassFile *cf)
{
	// copy vtable from parent
	if (this->parent != nullptr) {	// if this class is not java.lang.Object
		InstanceKlass *the_parent = ((InstanceKlass *)parent);
		this->vtable = the_parent->vtable;		// copy directly
	}

	// traverse all this.Methods
	wstringstream ss;
	for(int i = 0; i < cf->methods_count; i ++) {
		Method *method = new Method(this, cf->methods[i], cf->constant_pool);	// 采取把 cf.methods[i] 中的 attribute “移动语义” 移动到 Method 中的策略。这样 ClassFile 析构也不会影响到内部 Attributes 了。
		Method_Pool::put(method);		// put it into global area
		ss << method->get_name() << L":" << method->get_descriptor();		// save way: [name + ':' + descriptor]
		wstring signature = ss.str();
		// add method into [all methods]
		this->methods.insert(make_pair(signature, make_pair(i, method)));
		// override method into [vtable]
//		auto iter = std::find_if(vtable.begin(), vtable.end(), [method](Method *lhs){ return *method == *lhs; });	// 这个 lambda 还挺好看，留下了。
		auto iter = vtable.find(signature);
		if (iter == vtable.end()) {
			if (!method->is_static()/* && !method->is_private()*/)		// bug report: private 也是 vtable 的方法！！
				vtable.insert(make_pair(signature, method));
		} else {
			iter->second = method;		// override parent's method
		}

		ss.str(L"");		// make empty
	}
#ifdef KLASS_DEBUG
	sync_wcout{} << "===--------------- (" << this->get_name() << ") Debug Runtime MethodPool ---------------===" << std::endl;
	sync_wcout{} << "methods: total " << this->methods.size() << std::endl;
	int counter = 0;
	for (auto iter : this->methods) {
		sync_wcout{} << "  #" << counter++ << "  " << iter.first << std::endl;
	}
	sync_wcout{} << "===---------------------------------------------------------===" << std::endl;
#endif
}

void InstanceKlass::parse_constantpool(ClassFile *cf, ClassLoader *loader)
{
	this->rt_pool = new rt_constant_pool(this, loader, cf);
	Rt_Pool::put(this->rt_pool);
#ifdef KLASS_DEBUG
	// this has been deleted because lazy parsing constant_pool...
//	sync_wcout{} << "===--------------- (" << this->get_name() << ") Debug Runtime Constant Pool ---------------===" << std::endl;
//	this->rt_pool->print_debug();
//	std::cout << "===------------------------------------------------------------===" << std::endl;
#endif
}

void InstanceKlass::parse_attributes(ClassFile *cf)
{
	for(int i = 0; i < this->attributes_count; i ++) {
		int attribute_tag = peek_attribute(this->attributes[i]->attribute_name_index, cf->constant_pool);
		switch (attribute_tag) {	// must be 4, 5, 6, 7, 8, 9, 13, 14, 15, 18, 19, 21
			case 4: {	// InnerClasses
				inner_classes = (InnerClasses_attribute *)this->attributes[i];
				break;
			}
			case 5: {	// EnclosingMethod
				enclosing_method = (EnclosingMethod_attribute *)this->attributes[i];
				break;
			}
			case 7: {	// Signature
				signature_index = ((Signature_attribute *)this->attributes[i])->signature_index;
				break;
			}
			case 8: {	// SourceFile: 用于 printStackTrace。
				assert(cf->constant_pool[((SourceFile_attribute *)this->attributes[i])->sourcefile_index-1]->tag == CONSTANT_Utf8);
				sourceFile = ((CONSTANT_Utf8_info *)cf->constant_pool[((SourceFile_attribute *)this->attributes[i])->sourcefile_index-1])->convert_to_Unicode();
				break;
			}
			case 14:{		// RuntimeVisibleAnnotation
				auto & enter = ((RuntimeVisibleAnnotations_attribute *)this->attributes[i])->parameter_annotations;
				this->rva = (Parameter_annotations_t *)malloc(sizeof(Parameter_annotations_t));
				constructor(this->rva, cf->constant_pool, enter);
				break;
			}
			case 18:{		// RuntimeVisibleTypeAnnotation
				auto enter_ptr = ((RuntimeVisibleTypeAnnotations_attribute *)this->attributes[i]);
				this->num_RuntimeVisibleTypeAnnotations = enter_ptr->num_annotations;
				this->rvta = (TypeAnnotation *)malloc(sizeof(TypeAnnotation) * this->num_RuntimeVisibleTypeAnnotations);
				for (int pos = 0; pos < this->num_RuntimeVisibleTypeAnnotations; pos ++) {
					constructor(&this->rvta[pos], cf->constant_pool, enter_ptr->annotations[pos]);
				}
				break;
			}
			case 21:{		// BootStrapMethods (for lambda)
				this->bm = (BootstrapMethods_attribute *)this->attributes[i];
				break;
			}
			case 6:
			case 9:
			case 13:
			case 15:
			case 19:{
				break;
			}
			default:{
				std::cerr << "attribute_tag == " << attribute_tag << std::endl;
				assert(false);
			}
		}

	}
}

InstanceKlass::InstanceKlass(ClassFile *cf, ClassLoader *loader, MirrorOop *java_loader, ClassType classtype) : java_loader(java_loader), loader(loader), Klass()/*, classtype(classtype)*/
{
	this->classtype = classtype;
	// this_class (only name)
	assert(cf->constant_pool[cf->this_class-1]->tag == CONSTANT_Class);
	this->name = ((CONSTANT_Utf8_info *)cf->constant_pool[((CONSTANT_CS_info *)cf->constant_pool[cf->this_class-1])->index-1])->convert_to_Unicode();

	// move!!! important!!! move out Attributes.
	this->attributes = cf->attributes;
	cf->attributes = nullptr;

	// set to 0!!! important!!!
	this->attributes_count = cf->attributes_count;
	cf->attributes_count = 0;

	// set java_mirror
	java_lang_class::if_Class_didnt_load_then_delay(this, java_loader);

	// super_class
	parse_superclass(cf, loader);
	// become Runtime fields
	parse_fields(cf);			// fields must parse after the superclass. because it will copy super's fields.
	// this_class
	this->access_flags = cf->access_flags;
	// become Runtime interfaces
	parse_interfaces(cf, loader);
	// become Runtime methods
	parse_methods(cf);
	// become Runtime constant pool	// 必须放在构造函数之外！因为比如我加载 java.lang.Object，然后经由常量池加载了 java.lang.StringBuilder，注意此时 java.lang.Object 没被放到 system_classmap 中！然后又会加载 java.lang.Object，回来的时候会加载两遍 Object ！这是肯定不对的。于是设计成了丑恶的由 classloader 调用...QAQ
	parse_constantpool(cf, loader);
	// become Runtime Attributes
	parse_attributes(cf);

	// 收尾工作：清理即将死去的 cf，把好东西全都移动出来。(constant_pool)
	this->constant_pool = cf->constant_pool;
	cf->constant_pool = nullptr;
	this->constant_pool_count = cf->constant_pool_count;
	cf->constant_pool_count = 0;

}

pair<int, Field_info *> InstanceKlass::get_field(const wstring & descriptor)		// 字段解析时的查找
{
	pair<int, Field_info *> target = std::make_pair(-1, nullptr);
	InstanceKlass *instance_klass = this;
	while (instance_klass != nullptr) {
		wstring BIG_signature = instance_klass->get_name() + L":" + descriptor;
		// search in this->fields_layout
		auto iter = this->fields_layout.find(BIG_signature);
		if (iter == this->fields_layout.end()) {
			// search in this->static_fields_layout
			iter = instance_klass->static_fields_layout.find(descriptor);
		} else {
			return (*iter).second;
		}
		if (iter == instance_klass->static_fields_layout.end()) {
			// search in super_interfaces : reference Java SE 8 Specification $5.4.3.2: Parsing Fields
			for (auto iter : instance_klass->interfaces) {
				// TODO: 这些都没有考虑过 Interface 或者 parent 是 数组的情况.....感觉应当进行考虑...  虽然 Interface 我设置的默认是 InstanceKlass，不过 parent 可是 Klass...
//				target = iter.second->get_field(BIG_signature);		// fix the bug !!
				target = iter.second->get_field(descriptor);
				if (target.second != nullptr)	return target;
			}
			// search in super_class: parent : reference Java SE 8 Specification $5.4.3.2: Parsing Fields
			// TODO: 这里的强转会有问题！需要 Klass 实现一个方法返回这个 Klass 能不能是 InstanceKlass ！！暂时不考虑。等崩溃的时候再说。
//			if (this->parent != nullptr)	// this class is not java.lang.Object. java.lang.Object has no parent.
//				target = ((InstanceKlass *)this->parent)->get_field(BIG_signature);		// TODO: 这里暂时不是多态，因为没有虚方法。所以我改成了 static_pointer_cast。以后没准 InstanceKlass 要修改，需要注意。
//			return target;		// nullptr or Real result.
//			else {
			instance_klass = ((InstanceKlass *)instance_klass->get_parent());		// find by parent's BIG_signature
			continue;
//			}
		} else {
			return (*iter).second;
		}
	}
	return target;
}

bool InstanceKlass::get_static_field_value(Field_info *field, Oop **result)
{
	wstring signature = field->get_name() + L":" + field->get_descriptor();
	return get_static_field_value(signature, result);
}

void InstanceKlass::set_static_field_value(Field_info *field, Oop *value)
{
	wstring signature = field->get_name() + L":" + field->get_descriptor();
	set_static_field_value(signature, value);
}

bool InstanceKlass::get_static_field_value(const wstring & signature, Oop **result)				// use for forging String Oop at parsing constant_pool. However I don't no static field is of use ?
{
	auto iter = this->static_fields_layout.find(signature);
	if (iter == this->static_fields_layout.end()) {
		// ** search in parent for static !! **
		if (this->parent != nullptr)
			return ((InstanceKlass *)this->parent)->get_static_field_value(signature, result);
		else
			return false;
	}
	int offset = iter->second.first;
	*result = this->static_fields[offset];
	return true;
}

void InstanceKlass::set_static_field_value(const wstring & signature, Oop *value)
{
	auto iter = this->static_fields_layout.find(signature);
	if (iter == this->static_fields_layout.end()) {
		if (this->parent != nullptr) {
			((InstanceKlass *)this->parent)->set_static_field_value(signature, value);
			return;
		} else {
			std::cerr << "don't have this static field !!! fatal fault !!!" << std::endl;
			assert(false);
		}
	}
	int offset = iter->second.first;
	this->static_fields[offset] = value;
}

Method *InstanceKlass::get_this_class_method(const wstring & signature)
{
	auto iter = this->methods.find(signature);
	if (iter != this->methods.end())	{
		return (*iter).second.second;
	} else
		return nullptr;
}

Method *InstanceKlass::get_class_method(const wstring & signature, bool search_interfaces)
{
	assert(this->is_interface() == false);		// TODO: 此处的 verify 应该改成抛出异常。
	Method *target;
	// search in this->methods
//		std::wcout << "finding at: " << this->name << " find: " << signature << " and get method: " << std::endl;	// delete
//		std::cout << &this->methods << "  size:" << this->methods.size() << std::endl;
	auto iter = this->methods.find(signature);
	if (iter != this->methods.end())	{
		return (*iter).second.second;
	}
	// search in parent class (parent 既可以代表接口，又可以代表类。如果此类是接口，那么 parent 是接口。如果此类是个类，那么 parent 也是类。parent 完全按照 this 而定。)
	if (this->parent != nullptr)	// not java.lang.Object
		target = ((InstanceKlass *)this->parent)->get_class_method(signature);
	if (target != nullptr)	return target;
	// search in interfaces and interfaces' [parent interface].
	if (search_interfaces)		// this `switch` is for `invokeInterface`. Because `invokeInterface` only search in `this` and `parent`, not in `interfaces`.
		for (auto iter : this->interfaces) {
			target = iter.second->get_interface_method(signature);
			if (target != nullptr)	return target;
		}
	return nullptr;
}

Method *InstanceKlass::get_interface_method(const wstring & signature)
{
	if (this->name != L"java/lang/Object")			// 如果此类是 Object 类的话，默认也算作接口。	// 注意内部的分隔符变成了 '/' ！！
		assert(this->is_interface() == true);		// TODO: 此处的 verify 应该改成抛出异常。
	Method *target;
	// search in this->methods
	auto iter = this->methods.find(signature);
	if (iter != this->methods.end())	return (*iter).second.second;
	// search in parent interfaceS
	// 注意：接口使用 extends 代替 implements 关键字，可以有**多个父接口**！！
	for (auto iter : this->interfaces) {
//		std::wcout << this->name << " has interface: " << iter.first << std::endl;	// delete
//		std::wcout << this->name << "'s father: " << this->parent->get_name() << std::endl;	// delete
		target = iter.second->get_interface_method(signature);
		if (target != nullptr)	return target;
	}
	// search in parent... Big probability is java.lang.Object...
	if (this->parent != nullptr)	// this is not java.lang.Object
		target = ((InstanceKlass *)this->parent)->get_interface_method(signature);
	if (target != nullptr)	return target;

	return nullptr;
}

Method *InstanceKlass::get_static_void_main()
{
	for (auto iter : this->methods) {
		if (iter.second.second->is_main()) {
			return iter.second.second;
		}
	}
	assert(false);
}

void InstanceKlass::initialize_field(unordered_map<wstring, pair<int, Field_info *>> & fields_layout, vector<Oop *> & fields)
{
	for (auto & iter : fields_layout) {
		// ** 如果是 static 的 get_field，那么需要根据 signature 去 parent 去找。不过这里只是初始化，parent 的 static 域会被自己初始化，不用此子类管。 **
		int offset = iter.second.first;
		if (fields[offset] == nullptr) {			// 这里为 0，届时有可能是 ref 的 null，也有可能是 没有经过初始化 的 basic type oop.
			wstring type = iter.first.substr(iter.first.find_last_of(L":") + 1);		// e.g. value:[C --> [C,   value:J --> J
			if (type.size() == 1) {
				// is a basic type !!!
				switch (type[0]) {
					case L'B':	// byte
					case L'Z':	// boolean
					case L'S':	// short
					case L'C':	// char
					case L'I':	// int
						fields[offset] = new IntOop(0);
						break;
					case L'F':	// float
						fields[offset] = new FloatOop(0);
						break;
					case L'D':	// double
						fields[offset] = new DoubleOop(0);
						break;
					case L'J':	// long
						fields[offset] = new LongOop(0);
						break;
					default:{
						assert(false);
					}
				}
			}
		}
	}
}

// to set the ConstantValue_attribute at initialization.
void InstanceKlass::initialize_final_static_field()
{
	for (auto iter : this->static_fields_layout) {
		if (iter.second.second->is_static()) {		// TODO: ??? 很疑惑，我测试只有 static final 才需要强行设置 ConstantValue_attribute 啊...... 为何不是测试 final ?? 见 Spec... $4.7.2
			auto const_val_attr = iter.second.second->get_constant_value();
			if (const_val_attr != nullptr) {
				int index = const_val_attr->constantvalue_index;
//				std::wcout << "ConstantValue_attribute point to index: " << index << std::endl;		// delete
				auto _pair = (*this->rt_pool)[index - 1];
//				std::wcout << "initialize ConstantValue_attribute !" << std::endl;		// delete
				switch (_pair.first) {
					case CONSTANT_Long:{
						this->set_static_field_value(iter.first, new LongOop(boost::any_cast<long>(_pair.second)));
						break;
					}
					case CONSTANT_Float:{
						this->set_static_field_value(iter.first, new FloatOop(boost::any_cast<float>(_pair.second)));
						break;
					}
					case CONSTANT_Double:{
						this->set_static_field_value(iter.first, new DoubleOop(boost::any_cast<double>(_pair.second)));
						break;
					}
					case CONSTANT_Integer:{
						this->set_static_field_value(iter.first, new IntOop(boost::any_cast<int>(_pair.second)));
						break;
					}
					case CONSTANT_String:{
						this->set_static_field_value(iter.first, boost::any_cast<Oop *>(_pair.second));
						break;
					}
					default:{
						std::cerr << "it's ..." << _pair.first << std::endl;
						assert(false);
					}
				}
			}
		}
	}
}

wstring InstanceKlass::parse_signature()
{
	if (signature_index == 0) return L"";
	auto _pair = (*this->rt_pool)[signature_index];
	assert(_pair.first == CONSTANT_Utf8);
	wstring signature = boost::any_cast<wstring>(_pair.second);
	assert(signature != L"");	// 别和我设置为空而返回的 L"" 重了.....
	return signature;
}

wstring InstanceKlass::get_source_file_name()
{
//	assert(this->sourceFile != L"");
	return this->sourceFile;
}

bool InstanceKlass::check_interfaces(InstanceKlass *klass)
{
	return check_interfaces(klass->get_name());
}

bool InstanceKlass::check_interfaces(const wstring & signature)		// 查看此 InstanceKlass **是否实现了** signature 代表的 Interface.
{
	// 1. find in this class of the interface
	if (this->interfaces.find(signature) != this->interfaces.end())	return true;
	else {
		// 2. find in this class's interfaces (recursively) for the interface
		for (auto iter : this->interfaces) {		// recursive
			if (iter.second->check_interfaces(signature)) {
				return true;
			}
		}
	}
	// 3. then if `this`'s parent has the interface, also okay.
	if (this->get_parent() != nullptr)
		return ((InstanceKlass *)this->get_parent())->check_interfaces(signature);
	return false;
}

Method *InstanceKlass::search_vtable(const wstring & signature)
{
	auto iter = this->vtable.find(signature);
	if (iter == this->vtable.end()) {
		return nullptr;
	}
	return iter->second;
}

InstanceOop * InstanceKlass::new_instance() {
	return new InstanceOop(this);
}

InstanceKlass::~InstanceKlass() {
	for (int i = 0; i < attributes_count; i ++) {
		delete attributes[i];
	}
	delete[] attributes;

	destructor(this->rva);
	free(this->rva);

	for (int i = 0; i < num_RuntimeVisibleTypeAnnotations; i ++) {
		destructor(&rvta[i]);
	}
	free(rvta);

	for (int i = 0; i < constant_pool_count-1; i ++) {		// must -1!!
		int tag = constant_pool[i]->tag;
		delete constant_pool[i];
		if(tag == CONSTANT_Long || tag == CONSTANT_Double)	i ++;
	}
	delete[] constant_pool;
};

vector<pair<int, Method *>> InstanceKlass::get_constructors()
{
	vector<pair<int, Method *>> v;
	for (auto iter : this->methods) {
		if (iter.second.second->get_name() == L"<init>") {
			v.push_back(iter.second);
		}
	}
	assert(v.size() >= 1);
	return v;
}

vector<pair<int, Method *>> InstanceKlass::get_declared_methods()
{
	vector<pair<int, Method *>> v;
	for (auto iter : this->methods) {
		if (iter.second.second->get_name() == L"<clinit>") {
			continue;
		} else {
			v.push_back(iter.second);
		}
	}
	assert(v.size() >= 1);		// 至少要有一个构造函数。
	return v;
}

Method *InstanceKlass::search_method_in_slot(int slot)
{
	for (auto iter : this->methods) {
		if (iter.second.first == slot) {
			return iter.second.second;
		}
	}
	assert(false);
}

int InstanceKlass::get_static_field_offset(const wstring & signature)
{
	auto iter = this->static_fields_layout.find(signature);		// non-static field 由于复制了父类中的所有 field (继承)，所以只在 this_klass 中查找！
	if (iter == this->static_fields_layout.end()) {
		std::wcerr << "didn't find static field [" << signature << "] in InstanceKlass " << this->name << std::endl;
		assert(false);
	}
	int offset = iter->second.first;

#ifdef DEBUG
	sync_wcout{} << "this: [" << this << "], klass_name:[" << this->get_name() << "], (static)" << signature << ":[" << "(encoding: " << offset + this->non_static_field_num() << ")]" << std::endl;
#endif
	return offset + this->non_static_field_num();		// 这里需要注意。由于 static 和 non-static 我是分别存放的，而 unsafe 中指定的 offset 是唯一的。这就造成我不知道去 static 里边找还是 non-static 里边找。“两个都找，找到就ok” 的策略一定会引入软件漏洞。因此，采用编码，让 non-static 和 static 的编号永远不会重合。根据 non-static-field-size 来判断去哪里找。
}

int InstanceKlass::get_all_field_offset(const wstring & BIG_signature)
{
	auto iter = this->fields_layout.find(BIG_signature);		// non-static field 由于复制了父类中的所有 field (继承)，所以只在 this_klass 中查找！
	if (iter == this->fields_layout.end()) {
		// then, search in static field.
		return get_static_field_offset(BIG_signature.substr(BIG_signature.find_first_of(L":") + 1));
	}
	int offset = iter->second.first;

	// 不行，完全支持不了。java 默认所有 同一 klass 的 obj 都是相同的内存布局。我把 fields 挂在外边计算和 this 的距离，根本算不出来！每次都不同！！
	// [√] 只有一种手段可以解决 ———— 变绝对距离成相对距离，就像分布式系统变绝对时间为相对逻辑时间一样！！这样应该可以完美解决！！
	// 方法即是：
	// 这里存放的不是绝对距离，我会把语义完全改变，成为 “和此 oop 存放的 field 的起始地址的相对距离”，而不是 “和此 oop 的 this 指针的绝对距离”！！
	// 这样，GC 也可以用多种算法了！！看来也可以支持复制算法了！开森～
#ifdef DEBUG
	sync_wcout{} << "this: [" << this << "], klass_name:[" << this->get_name() << "], " << BIG_signature << ":[" << "(offset: " << offset <<")]" << std::endl;
#endif
//	return (char *)&this->fields[offset] - (char *)this;
	return offset;	// vector 是连续内存。
}

/*===---------------    MirrorKlass (aux)    --------------------===*/
MirrorOop *MirrorKlass::new_mirror(Klass *mirrored_who, MirrorOop *loader) {
	// 注意 mirrored_who 可以为 nullptr。因为在数组类使用了 new_mirror(nullptr, nullptr).
//	LockGuard lg(system_classmap_lock);		// bug report: 在指定 ./bin/wind_jvm Test18 的时候，Test18 根本没有的时候，java_lang_ClassLoader::find_bootstrap_class 方法中会 load sun/misc/launcher，然后在设置 mirror 的时候会冲突！但是即使设置了递归锁也没有用。所以我去掉了这两句无用的检测。
//	assert (system_classmap.find(L"java/lang/Class.class") != system_classmap.end());

	// then inject it!!
	if (mirrored_who != nullptr && mirrored_who->get_name() == L"java/lang/Exception") {
		auto & loaderr = BootStrapClassLoader::get_bootstrap();
		wstring ll(L"java/lang/Class");
		loaderr.loadClass(ll);
	}
	auto mirror = new MirrorOop(mirrored_who);
	if (loader != nullptr) {
		// need to initialize the `ClassLoader.class` by using ClassLoader's constructor!!
		mirror->set_field_value(L"java/lang/Class:classLoader:Ljava/lang/ClassLoader;", loader);
//		std::cerr << "**Please** load sun/misc/Launcher$AppClassLoader.class first !! " << std::endl;
	}

	return mirror;
}

/*===---------------    ArrayKlass    --------------------===*/
ArrayKlass::ArrayKlass(int dimension, ClassLoader *loader, Klass *lower_dimension, Klass *higher_dimension, MirrorOop *java_loader, ClassType classtype)  : dimension(dimension), loader(loader), lower_dimension(lower_dimension), higher_dimension(higher_dimension), java_loader(java_loader), Klass()/*, classtype(classtype)*/ {
	assert(dimension > 0);
	this->classtype = classtype;		// 这个变量不能放在初始化列表中初始化，即【不能用初始化列表直接初始化 不在基类构造函数参数列表 中的 基类的 protected 成员。】。会提示：error: member initializer 'classtype' does not name a non-static data member or base class
	// set super class
	this->set_parent(BootStrapClassLoader::get_bootstrap().loadClass(L"java/lang/Object"));
}

ArrayOop* ArrayKlass::new_instance(int length)
{
	ArrayOop *oop;
	if (this->get_type() == ClassType::TypeArrayClass)
		oop = new TypeArrayOop((TypeArrayKlass *)this, length);
	else
		oop = new ObjArrayOop((ObjArrayKlass *)this, length);
	for (int i = 0; i < length; i ++) {
		if (this->get_type() == ClassType::ObjArrayClass) {
			(*oop)[i] = nullptr;		// nullptr is the best !
		} else {		// allocate basic type...
			Type basic_type = ((TypeArrayKlass *)this)->get_basic_type();
			switch (basic_type) {
				case Type::BOOLEAN:					// 我全用 int[] 处理了。
				case Type::BYTE:
				case Type::SHORT:
				case Type::CHAR:
				case Type::INT:{
					(*oop)[i] = new IntOop(0);		// default value
					break;
				}
				case Type::DOUBLE:{
					(*oop)[i] = new DoubleOop(0);		// default value
					break;
				}
				case Type::FLOAT:{
					(*oop)[i] = new FloatOop(0);		// default value
					break;
				}
				case Type::LONG:{
					(*oop)[i] = new LongOop(0);		// default value
					break;
				}
				default:{
					assert(false);
				}
			}
		}
	}
	return oop;
}

/*===---------------  TypeArrayKlass  --------------------===*/
TypeArrayKlass::TypeArrayKlass(Type type, int dimension, ClassLoader *loader, Klass *lower_dimension, Klass *higher_dimension, MirrorOop *java_loader, ClassType classtype) : type(type), ArrayKlass(dimension, loader, lower_dimension, higher_dimension, java_loader, classtype)
{	// B:byte C:char D:double F:float I:int J:long S:short Z:boolean s: String e:enum c:Class @:Annotation [:Array
	// 1. get name
	wstringstream ss;		// 注：基本类型没有 enum 和 annotation。因为 enum 在 java 编译器处理之后，会被转型为 inner class。而 annotation 本质上就是普通的接口，相当于 class。所以基础类型没有他们。

	assert(loader == nullptr);

	for (int i = 0; i < dimension; i ++) {
		ss << L"[";
	}
	switch (type) {
		case Type::BOOLEAN:{
			ss << L"Z";
			break;
		}
		case Type::BYTE:{
			ss << L"B";
			break;
		}
		case Type::CHAR:{
			ss << L"C";
			break;
		}
		case Type::SHORT:{
			ss << L"S";
			break;
		}
		case Type::INT:{
			ss << L"I";
			break;
		}
		case Type::FLOAT:{
			ss << L"F";
			break;
		}
		case Type::LONG:{
			ss << L"J";
			break;
		}
		case Type::DOUBLE:{
			ss << L"D";
			break;
		}
		default:{
			std::cerr << "can't get here!" << std::endl;
			assert(false);
		}
	}
	this->name = ss.str();
	// set java_mirror
	java_lang_class::if_Class_didnt_load_then_delay(this, java_loader);
	// TODO: 要不要设置 object 的 child ...? 但是 sibling 的话，应该这个 higher 和 lower dimension 应该够 ???
}

ObjArrayKlass::ObjArrayKlass(InstanceKlass *element_klass, int dimension, ClassLoader *loader, Klass *lower_dimension, Klass *higher_dimension, MirrorOop *java_loader, ClassType classtype) : element_klass(element_klass), ArrayKlass(dimension, loader, lower_dimension, higher_dimension, java_loader, classtype)
{
	// 1. set name
	wstringstream ss;
	for (int i = 0; i < dimension; i ++) {
		ss << L"[";
	}
	ss << L"L" << element_klass->get_name() << L";";
	this->name = ss.str();
	// 2. set java_mirror
	java_lang_class::if_Class_didnt_load_then_delay(this, java_loader);
}

