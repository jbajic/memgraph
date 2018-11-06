#pragma once

#include "storage/distributed/mvcc/record.hpp"
#include "storage/distributed/mvcc/version_list.hpp"
#include "storage/common/types/property_value_store.hpp"
#include "storage/common/types/types.hpp"
#include "storage/distributed/address.hpp"

class Vertex;

class Edge : public mvcc::Record<Edge> {
  using VertexAddress = storage::Address<mvcc::VersionList<Vertex>>;

 public:
  Edge(VertexAddress from, VertexAddress to, storage::EdgeType edge_type)
      : from_(from), to_(to), edge_type_(edge_type) {}

  // Returns new Edge with copy of data stored in this Edge, but without
  // copying superclass' members.
  Edge *CloneData() { return new Edge(*this); }

  VertexAddress from_;
  VertexAddress to_;
  storage::EdgeType edge_type_;
  PropertyValueStore properties_;

 private:
  Edge(const Edge &other)
      : mvcc::Record<Edge>(),
        from_(other.from_),
        to_(other.to_),
        edge_type_(other.edge_type_),
        properties_(other.properties_) {}
};
