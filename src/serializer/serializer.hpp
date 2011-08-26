#ifndef __SERIALIZER_HPP__
#define __SERIALIZER_HPP__

#include <vector>

#include "utils.hpp"
#include <boost/optional.hpp>
#include <boost/variant/variant.hpp>

#include "arch/types.hpp"
#include "serializer/types.hpp"

#include "concurrency/cond_var.hpp"

struct index_write_op_t {
    block_id_t block_id;
    // Buf to write. None if not to be modified. Initialized but a null ptr if to be removed from lba.
    boost::optional<boost::intrusive_ptr<standard_block_token_t> > token;
    boost::optional<repli_timestamp_t> recency; // Recency, if it should be modified.
    boost::optional<bool> delete_bit;           // Delete bit, if it should be modified.

    index_write_op_t(block_id_t _block_id,
		     boost::optional<boost::intrusive_ptr<standard_block_token_t> > _token = boost::none,
		     boost::optional<repli_timestamp_t> _recency = boost::none,
		     boost::optional<bool> _delete_bit = boost::none)
	: block_id(_block_id), token(_token), recency(_recency), delete_bit(_delete_bit) {}
};

/* serializer_t is an abstract interface that describes how each serializer should
behave. It is implemented by log_serializer_t, semantic_checking_serializer_t, and
translator_serializer_t. */

struct serializer_t :
    /* Except as otherwise noted, the serializer's methods should only be called from the
    thread it was created on, and it should be destroyed on that same thread. */
    public home_thread_mixin_t
{
    typedef standard_block_token_t block_token_type;

    serializer_t() { }
    virtual ~serializer_t() { }

    /* The buffers that are used with do_read() and do_write() must be allocated using
    these functions. They can be safely called from any thread. */

    virtual void *malloc() = 0;
    virtual void *clone(void*) = 0; // clones a buf
    virtual void free(void*) = 0;

    /* Allocates a new io account for the underlying file.
    Use delete to free it. */
    file_account_t *make_io_account(int priority);
    virtual file_account_t *make_io_account(int priority, int outstanding_requests_limit) = 0;

    /* Some serializer implementations support read-ahead to speed up cache warmup.
    This is supported through a serializer_read_ahead_callback_t which gets called whenever the serializer has read-ahead some buf.
    The callee can then decide whether it wants to use the offered buffer of discard it.
    */
    virtual void register_read_ahead_cb(serializer_read_ahead_callback_t *cb) = 0;
    virtual void unregister_read_ahead_cb(serializer_read_ahead_callback_t *cb) = 0;

    /* Reading a block from the serializer */
    // Non-blocking variant
    virtual void block_read(const boost::intrusive_ptr<standard_block_token_t>& token, void *buf, file_account_t *io_account, iocallback_t *cb) = 0;

    // Blocking variant (requires coroutine context). Has default implementation.
    virtual void block_read(const boost::intrusive_ptr<standard_block_token_t>& token, void *buf, file_account_t *io_account) = 0;

    /* The index stores three pieces of information for each ID:
     * 1. A pointer to a data block on disk (which may be NULL)
     * 2. A repli_timestamp_t, called the "recency"
     * 3. A boolean, called the "delete bit" */

    /* max_block_id() and get_delete_bit() are used by the buffer cache to reconstruct
    the free list of unused block IDs. */

    /* Returns a block ID such that every existing block has an ID less than
     * that ID. Note that index_read(max_block_id() - 1) is not guaranteed to be
     * non-NULL. Note that for k > 0, max_block_id() - k might have never been
     * created. */
    virtual block_id_t max_block_id() = 0;

    /* Gets a block's timestamp.  This may return repli_timestamp_t::invalid. */
    virtual repli_timestamp_t get_recency(block_id_t id) = 0;

    /* Reads the block's delete bit. */
    virtual bool get_delete_bit(block_id_t id) = 0;

    /* Reads the block's actual data */
    virtual boost::intrusive_ptr<standard_block_token_t> index_read(block_id_t block_id) = 0;

    /* index_write() applies all given index operations in an atomic way */
    virtual void index_write(const std::vector<index_write_op_t>& write_ops, file_account_t *io_account) = 0;

    /* Non-blocking variants */
    virtual boost::intrusive_ptr<standard_block_token_t> block_write(const void *buf, block_id_t block_id, file_account_t *io_account, iocallback_t *cb) = 0;
    // `block_write(buf, acct, cb)` must behave identically to `block_write(buf, NULL_BLOCK_ID, acct, cb)`
    // a default implementation is provided using this
    virtual boost::intrusive_ptr<standard_block_token_t> block_write(const void *buf, file_account_t *io_account, iocallback_t *cb);

    /* Blocking variants (use in coroutine context) with and without known block_id */
    // these have default implementations in serializer.cc in terms of the non-blocking variants above
    virtual boost::intrusive_ptr<standard_block_token_t> block_write(const void *buf, file_account_t *io_account);
    virtual boost::intrusive_ptr<standard_block_token_t> block_write(const void *buf, block_id_t block_id, file_account_t *io_account);

    virtual block_sequence_id_t get_block_sequence_id(block_id_t block_id, const void* buf) = 0;

    // New do_write interface
    struct write_launched_callback_t {
        virtual void on_write_launched(const boost::intrusive_ptr<standard_block_token_t>& token) = 0;
        virtual ~write_launched_callback_t() {}
    };
    struct write_t {
        block_id_t block_id;
        struct update_t {
            const void *buf;
            repli_timestamp_t recency;
            iocallback_t *io_callback;
            write_launched_callback_t *launch_callback;
        };
        struct delete_t { char __unused_field; };
        struct touch_t { repli_timestamp_t recency; };
        // if none, indicates just a recency update.
        typedef boost::variant<update_t, delete_t, touch_t> action_t;
        action_t action;

        static write_t make_touch(block_id_t block_id, repli_timestamp_t recency);
        static write_t make_update(block_id_t block_id, repli_timestamp_t recency, const void *buf,
                                   iocallback_t *io_callback = NULL,
                                   write_launched_callback_t *launch_callback = NULL);
        static write_t make_delete(block_id_t block_id);
        write_t(block_id_t block_id, action_t action);
    };

    /* Performs a group of writes. Must be called from coroutine context. Returns when all writes
     * are finished and the LBA has been updated.
     *
     * Note that this is not virtual. It is implemented in terms of block_write() and index_write(),
     * and not meant to be overridden in subclasses.
     */
    void do_write(const std::vector<write_t>& writes, file_account_t *io_account);

    /* The size, in bytes, of each serializer block */

    virtual block_size_t get_block_size() = 0;

private:
    DISABLE_COPYING(serializer_t);
};


// Helpers for default implementations that can be used on log_serializer_t.

template <class serializer_type>
void serializer_index_write(serializer_type *ser, const index_write_op_t& op, file_account_t *io_account) {
    std::vector<index_write_op_t> ops;
    ops.push_back(op);
    return ser->index_write(ops, io_account);
}

template <class serializer_type>
boost::intrusive_ptr<typename serializer_traits_t<serializer_type>::block_token_type> serializer_block_write(serializer_type *ser, const void *buf, file_account_t *io_account, iocallback_t *cb) {
    return ser->block_write(buf, NULL_BLOCK_ID, io_account, cb);
}

// Blocking variants.
template <class serializer_type>
boost::intrusive_ptr<typename serializer_traits_t<serializer_type>::block_token_type> serializer_block_write(serializer_type *ser, const void *buf, file_account_t *io_account) {
    struct : public cond_t, public iocallback_t {
        void on_io_complete() { pulse(); }
    } cb;
    boost::intrusive_ptr<typename serializer_traits_t<serializer_type>::block_token_type> result = ser->block_write(buf, io_account, &cb);
    cb.wait();
    return result;
}

template <class serializer_type>
boost::intrusive_ptr<typename serializer_traits_t<serializer_type>::block_token_type> serializer_block_write(serializer_type *ser, const void *buf, block_id_t block_id, file_account_t *io_account) {
    struct : public cond_t, public iocallback_t {
        void on_io_complete() { pulse(); }
    } cb;
    boost::intrusive_ptr<typename serializer_traits_t<serializer_type>::block_token_type> result = ser->block_write(buf, block_id, io_account, &cb);
    cb.wait();
    return result;

}

#endif /* __SERIALIZER_HPP__ */
