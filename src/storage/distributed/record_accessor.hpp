/// @file
#pragma once

#include <glog/logging.h>

#include "storage/distributed/mvcc/version_list.hpp"
#include "storage/common/types/property_value.hpp"
#include "storage/common/types/property_value_store.hpp"
#include "storage/common/types/types.hpp"
#include "storage/distributed/address.hpp"
#include "storage/distributed/gid.hpp"
#include "utils/total_ordering.hpp"

namespace database {
class GraphDbAccessor;
struct StateDelta;
};  // namespace database

/**
 * An accessor to a database record (an Edge or a Vertex).
 *
 * Exposes view and update functions to the client programmer.
 * Assumes responsibility of doing all the relevant book-keeping
 * (such as index updates etc).
 *
 * @tparam TRecord Type of record (MVCC Version) of the accessor.
 */
template <typename TRecord>
class RecordAccessor : public utils::TotalOrdering<RecordAccessor<TRecord>> {
 public:
  using AddressT = storage::Address<mvcc::VersionList<TRecord>>;

  /**
   * Interface for the underlying implementation of the record accessor.
   * The RecordAccessor only borrows the pointer to the implementation, it does
   * *not* own it. When a RecordAccessor is copied, so is the pointer but *not*
   * the implementation itself. This means that concrete Impl types need to be
   * shareable among different accessors. To achieve that, it's best for derived
   * Impl types to contain *no state*. The reason we are using this approach is
   * to prevent large amounts of allocations, because RecordAccessor are often
   * created and copied.
   */
  class Impl {
   public:
    virtual ~Impl() {}

    virtual AddressT GlobalAddress(const RecordAccessor<TRecord> &ra) = 0;
    /** Set the pointers for old and new records during `Reconstruct`. */
    virtual void SetOldNew(const RecordAccessor<TRecord> &ra,
                           TRecord **old_record, TRecord **new_record) = 0;
    /** Find the pointer to the new, updated record. */
    virtual TRecord *FindNew(const RecordAccessor<TRecord> &ra) = 0;
    /** Process a change delta, e.g. by writing WAL. */
    virtual void ProcessDelta(const RecordAccessor<TRecord> &ra,
                              const database::StateDelta &delta) = 0;
    virtual int64_t CypherId(const RecordAccessor<TRecord> &ra) = 0;
  };

  // this class is default copyable, movable and assignable
  RecordAccessor(const RecordAccessor &other) = default;
  RecordAccessor(RecordAccessor &&other) = default;
  RecordAccessor &operator=(const RecordAccessor &other) = default;
  RecordAccessor &operator=(RecordAccessor &&other) = default;

 protected:
  /**
   * Protected destructor because we allow inheritance, but nobody should own a
   * pointer to plain RecordAccessor.
   */
  ~RecordAccessor() = default;

  /**
   * Only derived types may allow construction.
   *
   * @param address Address (local or global) of the Vertex/Edge of this
   * accessor.
   * @param db_accessor The DB accessor that "owns" this record accessor.
   * @param impl Borrowed pointer to the underlying implementation.
   */
  RecordAccessor(AddressT address, database::GraphDbAccessor &db_accessor,
                 Impl *impl);

 public:
  /** Gets the property for the given key. */
  PropertyValue PropsAt(storage::Property key) const;

  /** Sets a value on the record for the given property. */
  void PropsSet(storage::Property key, PropertyValue value);

  /** Erases the property for the given key. */
  void PropsErase(storage::Property key);

  /** Removes all the properties from this record. */
  void PropsClear();

  /** Returns the properties of this record. */
  const PropertyValueStore &Properties() const;

  bool operator==(const RecordAccessor &other) const;

  /** Returns a GraphDB accessor of this record accessor. */
  database::GraphDbAccessor &db_accessor() const;

  /**
   * Returns a globally-unique ID of this vertex or edge. Note that vertices
   * and edges have separate ID domains, there can be a vertex with ID X and an
   * edge with the same id.
   */
  gid::Gid gid() const;

  AddressT address() const;

  AddressT GlobalAddress() const;

  /*
   * Switches this record accessor to use the latest version visible to the
   * current transaction+command.  Possibly the one that was created by this
   * transaction+command.
   *
   * @return A reference to this.
   */
  RecordAccessor<TRecord> &SwitchNew();

  /** Returns the new record pointer. */
  TRecord *GetNew() const { return new_; }

  /**
   * Attempts to switch this accessor to use the latest version not updated by
   * the current transaction+command.  If that is not possible (vertex/edge was
   * created by the current transaction/command), it does nothing (current
   * remains pointing to the new version).
   *
   * @return A reference to this.
   */
  RecordAccessor<TRecord> &SwitchOld();

  /** Returns the old record pointer. */
  TRecord *GetOld() const { return old_; }

  /**
   * Reconstructs the internal state of the record accessor so it uses the
   * versions appropriate to this transaction+command.
   *
   * @return True if this accessor is valid after reconstruction.  This means
   * that at least one record pointer was found (either new_ or old_), possibly
   * both.
   */
  bool Reconstruct() const;

  /**
   * Ensures there is an updateable version of the record in the version_list,
   * and that the `new_` pointer points to it. Returns a reference to that
   * version.
   *
   * It is not legal to call this function on a Vertex/Edge that has been
   * deleted in the current transaction+command.
   *
   * @throws RecordDeletedError
   */
  TRecord &update() const;

  /**
   * Returns true if the given accessor is visible to the given transaction.
   *
   * @param current_state If true then the graph state for the
   *    current transaction+command is returned (insertions, updates and
   *    deletions performed in the current transaction+command are not
   *    ignored).
   */
  bool Visible(const tx::Transaction &t, bool current_state) const {
    return (old_ && !(current_state && old_->is_expired_by(t))) ||
           (current_state && new_ && !new_->is_expired_by(t));
  }

  // TODO: This shouldn't be here, because it's only relevant in distributed.
  /** Indicates if this accessor represents a local Vertex/Edge, or one whose
   * owner is some other worker in a distributed system. */
  bool is_local() const { return address_.is_local(); }

  /**
   * Returns Cypher Id of this record.
   */
  int64_t CypherId() const;

 protected:
  /**
   * The database::GraphDbAccessor is friend to this accessor so it can
   * operate on it's data (mvcc version-list and the record itself).
   * This is legitimate because database::GraphDbAccessor creates
   * RecordAccessors
   * and is semantically their parent/owner. It is necessary because
   * the database::GraphDbAccessor handles insertions and deletions, and these
   * operations modify data intensively.
   */
  friend database::GraphDbAccessor;

  /** Process a change delta, e.g. by writing WAL. */
  void ProcessDelta(const database::StateDelta &delta) const;

  /**
   * Pointer to the version (either old_ or new_) that READ operations
   * in the accessor should take data from. Note that WRITE operations
   * should always use new_.
   *
   * This pointer can be null if created by an accessor which lazily reads from
   * mvcc.
   */
  mutable TRecord *current_{nullptr};

  /** Returns the current version (either new_ or old_) set on this
   * RecordAccessor. */
  const TRecord &current() const;

 private:
  Impl *impl_;
  // The database accessor for which this record accessor is created
  // Provides means of getting to the transaction and database functions.
  // Immutable, set in the constructor and never changed.
  database::GraphDbAccessor *db_accessor_;

  AddressT address_;

  /**
   * Latest version which is visible to the current transaction+command
   * but has not been created nor modified by the current transaction+command.
   *
   * Can be null only when the record itself (the version-list) has
   * been created by the current transaction+command.
   */
  mutable TRecord *old_{nullptr};

  /**
   * Version that has been modified (created or updated) by the current
   * transaction+command.
   *
   * Can be null when the record has not been modified in the current
   * transaction+command. It is also possible that the modification
   * has happened, but this RecordAccessor does not know this. To
   * ensure correctness, the `SwitchNew` function must check if this
   * is null, and if it is it must check with the vlist_ if there is
   * an update.
   */
  mutable TRecord *new_{nullptr};
};

/** Error when trying to update a deleted record */
class RecordDeletedError : public utils::BasicException {
 public:
  RecordDeletedError()
      : utils::BasicException(
            "Can't update a record deleted in the current transaction+commad") {
  }
};
