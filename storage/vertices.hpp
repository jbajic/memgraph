#pragma once

#include "vertex.hpp"
#include "common.hpp"

class Vertices
{
public:
    const Vertex* find(tx::Transaction& transaction, const Id& id)
    {
        // find vertex record
        auto vertices_accessor = vertices.access();
        auto vertex_record = vertices_accessor.find(id);

        if (vertex_record == vertices_accessor.end())
            return nullptr;

        // find vertex
        auto vertex_accessor = vertex_record->second.access(transaction);
        auto vertex = vertex_accessor.find();

        return vertex;
    }

    Vertex* insert(tx::Transaction& transaction)
    {
        // get next vertex id
        auto next = counter.next(std::memory_order_acquire);

        // create new vertex record
        VertexRecord vertex_record;

        // insert the new vertex record into the vertex store
        auto vertices_accessor = vertices.access();
        auto result = vertices_accessor.insert_unique(next, std::move(vertex_record)); 

        // create new vertex
        auto inserted_vertex_record = result.first;
        auto vertex_accessor = inserted_vertex_record->second.access(transaction);
        auto vertex = vertex_accessor.insert();

        return vertex;
    }

    Vertex* update(tx::Transaction& transaction, const Id& id)
    {
        // find vertex record
        auto vertices_accessor = vertices.access();
        auto vertex_record = vertices_accessor.find(id);

        if (vertex_record == vertices_accessor.end())
            return nullptr;

        // get vertex that is going to be updated
        auto vertex_accessor = vertex_record->second.access(transaction);
        auto vertex = vertex_accessor.update();

        return vertex;
    }

    bool remove(tx::Transaction& transaction, const Id& id)
    {
        // find vertex record
        auto vertices_accessor = vertices.access();
        auto vertex_record = vertices_accessor.find(id);

        if (vertex_record == vertices_accessor.end())
            return false;

        // mark vertex record via the vertex accessor as deleted
        // return boolean result true if vertex could be removed
        // or false if vertex couldn't be removed
        auto vertex_accessor = vertex_record->second.access(transaction);
        return vertex_accessor.remove();
    }

private:
    SkipList<uint64_t, VertexRecord> vertices;
    AtomicCounter<uint64_t> counter;
};

