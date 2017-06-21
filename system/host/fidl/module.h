// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <errno.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"
#include "identifier_table.h"
#include "source_manager.h"

namespace fidl {

template <typename T>
class Scope {
public:
    bool Insert(const T& t) {
        auto iter = scope_.insert(t);
        return iter.second;
    }

private:
    std::set<T> scope_;
};

// After name and type consumption of the AST, these types are no longer nested.
class Ordinal {
public:
    Ordinal(std::unique_ptr<NumericLiteral> literal, uint32_t value) :
        literal_(std::move(literal)),
        value_(value) {}

    uint32_t Value() const { return value_; }

private:
    std::unique_ptr<NumericLiteral> literal_;
    uint32_t value_;
};

class Name {
public:
    Name() : name_(nullptr) {}

    explicit Name(std::unique_ptr<Identifier> name) :
        name_(std::move(name)) {
    }

    StringView data() const {
        if (!name_)
            return StringView();
        return name_->identifier.data();
    }

private:
    std::unique_ptr<Identifier> name_;
};

struct ConstInfo {
    ConstInfo(Name name, std::unique_ptr<Type> type, std::unique_ptr<Constant> value) :
        name(std::move(name)),
        type(std::move(type)),
        value(std::move(value)) {}
    Name name;
    std::unique_ptr<Type> type;
    std::unique_ptr<Constant> value;
};

struct EnumInfo {
    struct Member {
        Member(Name name, std::unique_ptr<Constant> value) :
            name(std::move(name)),
            value(std::move(value)) {}
        Name name;
        std::unique_ptr<Constant> value;
    };

    EnumInfo(Name name, std::unique_ptr<PrimitiveType> type, std::vector<Member> members) :
        name(std::move(name)),
        type(std::move(type)),
        members(std::move(members)) {}

    Name name;
    std::unique_ptr<PrimitiveType> type;
    std::vector<Member> members;
};

struct InterfaceInfo {
    struct Method {
        struct Parameter {
            Parameter(std::unique_ptr<Type> type, Name name) :
                type(std::move(type)),
                name(std::move(name)) {}
            std::unique_ptr<Type> type;
            Name name;
        };

        Method(Method&&) = default;
        Method& operator=(Method&&) = default;

        Method(Ordinal ordinal, Name name, std::vector<Parameter> parameter_list,
               bool has_response, std::vector<Parameter> maybe_response) :
            ordinal(std::move(ordinal)),
            name(std::move(name)),
            parameter_list(std::move(parameter_list)),
            has_response(has_response),
            maybe_response(std::move(maybe_response)) {}

        Ordinal ordinal;
        Name name;
        std::vector<Parameter> parameter_list;
        bool has_response;
        std::vector<Parameter> maybe_response;
    };

    InterfaceInfo(Name name, std::vector<Method> methods) :
        name(std::move(name)),
        methods(std::move(methods)) {}

    Name name;
    std::vector<Method> methods;
};

struct StructInfo {
    struct Member {
        Member(std::unique_ptr<Type> type, Name name, std::unique_ptr<Constant> default_value) :
            type(std::move(type)),
            name(std::move(name)),
            default_value(std::move(default_value)) {}
        std::unique_ptr<Type> type;
        Name name;
        std::unique_ptr<Constant> default_value;
    };

    StructInfo(Name name, std::vector<Member> members) :
        name(std::move(name)),
        members(std::move(members)) {}

    Name name;
    std::vector<Member> members;
};

struct UnionInfo {
    struct Member {
        Member(std::unique_ptr<Type> type, Name name) :
            type(std::move(type)),
            name(std::move(name)) {}
        std::unique_ptr<Type> type;
        Name name;
    };

    UnionInfo(Name name, std::vector<Member> members) :
        name(std::move(name)),
        members(std::move(members)) {}

    Name name;
    std::vector<Member> members;
};

class TypeShape;

// Represents an out-of-line allocation.
class Allocation {
public:
    explicit Allocation(TypeShape typeshape, size_t bound = std::numeric_limits<size_t>::max()) :
        size_(size),
        alignment_(alignment),
        bound_(bound) {
        // Must be a power of 2.
        assert(((alignment_ & (alignment_ - 1)) == 0) && alignment_ != 0);
    }
    Allocation() : Allocation(0u, 1u) {}

    size_t Size() { return typeshape.Size(); }
    size_t Alignment() { return typeshape.Alignment(); }
    const std::vector<Allocation>& Allocations() { return typeshape.Allocations(); }
    size_t Bound() { return bound_; }

private:
    TypeShape typeshape;
    size_t bound_;
};

// Represents a type in a message. For example, for the following
//
//     struct tag {
//         int32 t;
//     }
//     struct vectors_of_vectors {
//         vector<vector<tag?>:3> tags;
//         array<handle<channel>>:8 channels;
//         vector<vector<uint32>>:5 ints;
//     }
//
// the typeshape corresponding to vectors_of_vectors is
//
//     TypeShape {
//         size_ = 64 // 16 + (8 * 4) + 16
//         alignment_ = 8 // The pointers and sizes of the vectors are 8 byte aligned.
//         allocations_ = {
//             Allocation { // The allocation for an unbounded vector of bounded vectors...
//                 typeshape_ = {
//                     size_ = 16
//                     alignment_ = 8
//                     allocations_ = {
//                         Allocation { // ...each of which is a pointer to a struct tag.
//                             typeshape_ = {
//                                 size_ = 8
//                                 alignment_ = 8
//                                 allocations_ = {
//                                     Allocation {
//                                         typeshape_ = {
//                                             size_ = 4
//                                             alignment_ = 4
//                                             allocations_ = {
//                                             }
//                                         }
//                                         bound_ = 1
//                                     }
//                                 }
//                             bound_ = 3
//                             }
//                         }
//                     }
//                 bound_ = SIZE_MAX
//             }
//             Allocation { // The allocation for a bounded vector of unbounded vectors...
//                 typeshape_ = {
//                     size_ = 16
//                     alignment_ = 8
//                     allocations_ = {
//                         Allocation { // ...each of which is a uint32.
//                             typeshape_ = {
//                                 size_ = 4
//                                 alignment_ = 4
//                                 allocations_ = {
//                                 }
//                             bound_ = SIZE_MAX
//                             }
//                         }
//                     }
//                 bound_ = 5
//             }
//         }
//     }
class TypeShape {
public:
    TypeShape(size_t size, size_t alignment, std::vector<Allocation> allocations) :
        size_(size),
        alignment_(alignment),
        allocations_(std::move(allocations)) {
        // Must be a power of 2.
        assert(((alignment_ & (alignment_ - 1)) == 0) && alignment_ != 0);
    }
    TypeShape(size_t size, size_t alignment) : TypeShape(size, alignment, std::vector<Allocation>()) {}
    TypeShape() : size_(0u), alignment_(1u) {}

    size_t Size() { return size_; }
    size_t Alignment() { return alignment_; }
    const std::vector<Allocation>& Allocations() { return allocations_; }

    void AddAllocation(Allocation allocation) {
        allocations_.push_back(allocation);
    }

private:
    size_t size_;
    size_t alignment_;
    std::vector<Allocation> allocations_;
};

class Module {
public:
    bool CreateSource(const char* file_name, StringView* source_out) {
        return source_manager_.CreateSource(file_name, source_out);
    }
    bool Parse(StringView source);
    bool Dump();

private:
    bool ConsumeConstDeclaration(std::unique_ptr<ConstDeclaration> const_declaration);
    bool ConsumeEnumDeclaration(std::unique_ptr<EnumDeclaration> enum_declaration);
    bool ConsumeInterfaceDeclaration(std::unique_ptr<InterfaceDeclaration> interface_declaration);
    bool ConsumeStructDeclaration(std::unique_ptr<StructDeclaration> struct_declaration);
    bool ConsumeUnionDeclaration(std::unique_ptr<UnionDeclaration> union_declaration);
    bool ConsumeFile(std::unique_ptr<File> file);

    bool RegisterTypeName(const Name& name);

    bool ResolveConstInfo(const ConstInfo& const_info);
    bool ResolveEnumInfo(const EnumInfo& enum_info);
    bool ResolveInterfaceInfo(const InterfaceInfo& interface_info);
    bool ResolveStructInfo(const StructInfo& struct_info);
    bool ResolveUnionInfo(const UnionInfo& union_info);
    bool Resolve();

    bool ResolveArrayType(const ArrayType& array_type, TypeShape* out_type_metadata);
    bool ResolveVectorType(const VectorType& vector_type, TypeShape* out_type_metadata);
    bool ResolveStringType(const StringType& string_type, TypeShape* out_type_metadata);
    bool ResolveHandleType(const HandleType& handle_type, TypeShape* out_type_metadata);
    bool ResolveRequestType(const RequestType& request_type, TypeShape* out_type_metadata);
    bool ResolvePrimitiveType(const PrimitiveType& primitive_type, TypeShape* out_type_metadata);
    bool ResolveIdentifierType(const IdentifierType& identifier_type, TypeShape* out_type_metadata);
    bool ResolveType(const Type* type) {
        TypeShape type_metadata;
        return ResolveType(type, &type_metadata);
    }
    bool ResolveType(const Type* type, TypeShape* out_type_metadata);
    bool ResolveTypeName(const CompoundIdentifier* name);
    bool RegisterResolvedType(const Name& name, TypeShape type_metadata);

    std::vector<ConstInfo> const_infos_;
    std::vector<EnumInfo> enum_infos_;
    std::vector<InterfaceInfo> interface_infos_;
    std::vector<StructInfo> struct_infos_;
    std::vector<UnionInfo> union_infos_;

    std::set<StringView> registered_types_;
    std::map<StringView, TypeShape> resolved_types_;

    std::vector<std::set<StringView>> scoped_names_;

    IdentifierTable identifier_table_;
    SourceManager source_manager_;
};

} // namespace fidl
