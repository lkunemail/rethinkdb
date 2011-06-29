
#include "btree/modify_oper.hpp"

#include "utils.hpp"
#include <boost/shared_ptr.hpp>
#include "buffer_cache/buf_lock.hpp"
#include "buffer_cache/co_functions.hpp"
#include "btree/leaf_node.hpp"
#include "btree/internal_node.hpp"

#include "buffer_cache/co_functions.hpp"
#include "slice.hpp"

// TODO: consider B#/B* trees to improve space efficiency

// TODO: perhaps allow memory reclamation due to oversplitting? We can
// be smart and only use a limited amount of ram for incomplete nodes
// (doing this efficiently very tricky for high insert
// workloads). Also, if the serializer is log-structured, we can write
// only a small part of each node.

// TODO: change rwi_write to rwi_intent followed by rwi_upgrade where
// relevant.

void insert_root(block_id_t root_id, buf_lock_t& sb_buf) {
    rassert(sb_buf.is_acquired());
    // TODO: WTF is up with this const cast?  This makes NO SENSE.  gtfo.
    sb_buf->set_data(const_cast<block_id_t *>(&reinterpret_cast<const btree_superblock_t *>(sb_buf->get_data_read())->root_block), &root_id, sizeof(root_id));

    sb_buf.release();
}

// Split the node if necessary. If the node is a leaf_node, provide the new
// value that will be inserted; if it's an internal node, provide NULL (we
// split internal nodes proactively).
void check_and_handle_split(transaction_t *txn, buf_lock_t& buf, buf_lock_t& last_buf, buf_lock_t& sb_buf,
                            const btree_key_t *key, btree_value_t *new_value, block_size_t block_size) {
    txn->assert_thread();

    const node_t *node = reinterpret_cast<const node_t *>(buf->get_data_read());

    // If the node isn't full, we don't need to split, so we're done.
    if (node::is_leaf(node)) { // This should only be called when update_needed.
        rassert(new_value);
        if (!leaf::is_full(txn->get_cache()->get_block_size(), reinterpret_cast<const leaf_node_t *>(node), key, new_value)) return;
    } else {
        rassert(!new_value);
        if (!internal_node::is_full(reinterpret_cast<const internal_node_t *>(node))) return;
    }

    // Allocate a new node to split into, and some temporary memory to keep
    // track of the median key in the split; then actually split.
    buf_lock_t rbuf;
    rbuf.allocate(txn);
    btree_key_buffer_t median_buffer;
    btree_key_t *median = median_buffer.key();

    node::split(block_size, *buf.buf(), *rbuf.buf(), median);

    // Insert the key that sets the two nodes apart into the parent.
    if (!last_buf.is_acquired()) {
        // We're splitting what was previously the root, so create a new root to use as the parent.
        last_buf.allocate(txn);
        internal_node::init(block_size, *last_buf.buf());

        insert_root(last_buf->get_block_id(), sb_buf);
    }

    bool success __attribute__((unused)) = internal_node::insert(block_size, *last_buf.buf(), median, buf->get_block_id(), rbuf->get_block_id());
    rassert(success, "could not insert internal btree node");

    // We've split the node; now figure out where the key goes and release the other buf (since we're done with it).
    if (0 >= sized_strcmp(key->contents, key->size, median->contents, median->size)) {
        // The key goes in the old buf (the left one).

        // Do nothing.

    } else {
        // The key goes in the new buf (the right one).
        buf.swap(rbuf);
    }
}

// Merge or level the node if necessary.
void check_and_handle_underfull(transaction_t *txn, buf_lock_t& buf, buf_lock_t& last_buf, buf_lock_t& sb_buf,
                                const btree_key_t *key, block_size_t block_size) {
    const node_t *node = reinterpret_cast<const node_t *>(buf->get_data_read());
    if (last_buf.is_acquired() && node::is_underfull(block_size, node)) { // The root node is never underfull.

        const internal_node_t *parent_node = reinterpret_cast<const internal_node_t *>(last_buf->get_data_read());

        // Acquire a sibling to merge or level with.
        block_id_t sib_node_id;
        int nodecmp_node_with_sib = internal_node::sibling(parent_node, key, &sib_node_id);

        // Now decide whether to merge or level.
        buf_lock_t sib_buf(txn, sib_node_id, rwi_write);
        const node_t *sib_node = reinterpret_cast<const node_t *>(sib_buf->get_data_read());

#ifndef NDEBUG
        node::validate(block_size, sib_node);
#endif

        if (node::is_mergable(block_size, node, sib_node, parent_node)) { // Merge.

            // This is the key that we remove.
            btree_key_buffer_t key_to_remove_buffer;
            btree_key_t *key_to_remove = key_to_remove_buffer.key();

            if (nodecmp_node_with_sib < 0) { // Nodes must be passed to merge in ascending order.
                node::merge(block_size, node, *sib_buf.buf(), key_to_remove, parent_node);
                buf->mark_deleted();
                buf.swap(sib_buf);
            } else {
                node::merge(block_size, sib_node, *buf.buf(), key_to_remove, parent_node);
                sib_buf->mark_deleted();
            }

            sib_buf.release();

            if (!internal_node::is_singleton(parent_node)) {
                internal_node::remove(block_size, *last_buf.buf(), key_to_remove);
            } else {
                // The parent has only 1 key after the merge (which means that
                // it's the root and our node is its only child). Insert our
                // node as the new root.
                last_buf->mark_deleted();
                insert_root(buf->get_block_id(), sb_buf);
            }
        } else { // Level
            btree_key_buffer_t key_to_replace_buffer, replacement_key_buffer;
            btree_key_t *key_to_replace = key_to_replace_buffer.key();
            btree_key_t *replacement_key = replacement_key_buffer.key();

            bool leveled = node::level(block_size, *buf.buf(), *sib_buf.buf(), key_to_replace, replacement_key, parent_node);

            if (leveled) {
                internal_node::update_key(*last_buf.buf(), key_to_replace, replacement_key);
            }
        }
    }
}

// Get a root block given a superblock, or make a new root if there isn't one.
void get_root(transaction_t *txn, buf_lock_t& sb_buf, block_size_t block_size, buf_lock_t *buf_out, repli_timestamp timestamp) {
    rassert(!buf_out->is_acquired());

    block_id_t node_id = reinterpret_cast<const btree_superblock_t*>(sb_buf->get_data_read())->root_block;

    if (node_id != NULL_BLOCK_ID) {
        buf_lock_t tmp(txn, node_id, rwi_write);
        buf_out->swap(tmp);
    } else {
        buf_out->allocate(txn);
        leaf::init(block_size, *buf_out->buf(), timestamp);
        insert_root(buf_out->buf()->get_block_id(), sb_buf);
    }
}

// Runs a btree_modify_oper_t.
void run_btree_modify_oper(btree_modify_oper_t *oper, btree_slice_t *slice, const store_key_t &store_key, castime_t castime, order_token_t token) {
    scoped_malloc<btree_value_t> the_value(MAX_BTREE_VALUE_SIZE);
    memset(the_value.get(), 0, MAX_BTREE_VALUE_SIZE);

    btree_key_buffer_t kbuffer(store_key);
    btree_key_t *key = kbuffer.key();

    block_size_t block_size = slice->cache()->get_block_size();

    {
        slice->assert_thread();

        slice->pre_begin_transaction_sink_.check_out(token);
        order_token_t begin_transaction_token = slice->pre_begin_transaction_write_mode_source_.check_in(token.tag() + "+begin_transaction_token");

        transaction_t txn(slice->cache(), rwi_write, oper->compute_expected_change_count(slice->cache()->get_block_size().value()),  castime.timestamp);

        slice->post_begin_transaction_sink_.check_out(begin_transaction_token);

	txn.set_token(slice->post_begin_transaction_source_.check_in(token.tag() + "+post"));

        buf_lock_t sb_buf(&txn, SUPERBLOCK_ID, rwi_write);

        // TODO: do_superblock_sidequest is blocking.  It doesn't have
        // to be, but when you fix this, make sure the superblock
        // sidequest is done using the superblock before the
        // superblock gets released.
        oper->do_superblock_sidequest(&txn, sb_buf, castime.timestamp, &store_key);

        buf_lock_t last_buf;
        buf_lock_t buf;
        get_root(&txn, sb_buf, block_size, &buf, castime.timestamp);

        // Walk down the tree to the leaf.
        while (node::is_internal(reinterpret_cast<const node_t *>(buf->get_data_read()))) {
            // Check if the node is overfull and proactively split it if it is (since this is an internal node).
            check_and_handle_split(&txn, buf, last_buf, sb_buf, key, NULL, block_size);
            // Check if the node is underfull, and merge/level if it is.
            check_and_handle_underfull(&txn, buf, last_buf, sb_buf, key, block_size);

            // Release the superblock, if we've gone past the root (and haven't
            // already released it). If we're still at the root or at one of
            // its direct children, we might still want to replace the root, so
            // we can't release the superblock yet.
            if (sb_buf.is_acquired() && last_buf.is_acquired()) {
                sb_buf.release();
            }

            // Release the old previous node (unless we're at the root), and set
            // the next previous node (which is the current node).

            // Look up and acquire the next node.
            block_id_t node_id = internal_node::lookup(reinterpret_cast<const internal_node_t *>(buf->get_data_read()), key);
            rassert(node_id != NULL_BLOCK_ID && node_id != SUPERBLOCK_ID);

            buf_lock_t tmp(&txn, node_id, rwi_write);
            last_buf.swap(tmp);
            buf.swap(last_buf);
        }

        // We've gone down the tree and gotten to a leaf. Now look up the key.
        bool key_found = leaf::lookup(block_size, reinterpret_cast<const leaf_node_t *>(buf->get_data_read()), key, the_value.get());

        bool expired = key_found && the_value->expired();
        if (expired) key_found = false;

        // If the value's expired, delete it.
        if (expired) {
            blob_t b(the_value->value_ref(), blob::btree_maxreflen);
            b.unappend_region(&txn, b.valuesize());
            the_value.reset();
        }
        bool update_needed = oper->operate(&txn, the_value);

        // If the value is expired and operate() decided not to make any
        // change, we'll silently delete the key.
        if (!update_needed && expired) {
            rassert(!the_value);
            update_needed = true;
        }

        // Actually update the leaf, if needed.
        if (update_needed) {
            if (the_value) { // We have a value to insert.
                // Split the node if necessary, to make sure that we have room
                // for the value; This isn't necessary when we're deleting,
                // because the node isn't going to grow.
                check_and_handle_split(&txn, buf, last_buf, sb_buf, key, the_value.get(), block_size);

                // Add a CAS to the value if necessary (this won't change its size).
                if (the_value->has_cas()) {
                    rassert(castime.proposed_cas != BTREE_MODIFY_OPER_DUMMY_PROPOSED_CAS);
                    the_value->set_cas(block_size, castime.proposed_cas);
                }

                repli_timestamp new_value_timestamp = castime.timestamp;

                bool success = leaf::insert(block_size, *buf.buf(), key, the_value.get(), new_value_timestamp);
                guarantee(success, "could not insert leaf btree node");
            } else { // Delete the value if it's there.
                if (key_found || expired) {
                    leaf::remove(block_size, *buf.buf(), key);
                } else {
                     // operate() told us to delete a value (update_needed && !the_value), but the
                     // key wasn't in the node (!key_found && !expired), so we do nothing.
                }
            }

            // XXX: Previously this was checked whether or not update_needed,
            // but I'm pretty sure a leaf node can only be underfull
            // immediately following a split or an update. Double check this.

            // Check to see if the leaf is underfull (following a change in
            // size or a deletion), and merge/level if it is.
            check_and_handle_underfull(&txn, buf, last_buf, sb_buf, key, block_size);
        }

        // Release bufs as necessary.
        sb_buf.release_if_acquired();
        rassert(buf.is_acquired());
        buf.release();
        last_buf.release_if_acquired();

        // Committing the transaction and moving back to the home thread are
        // handled automatically with RAII.
    }
}
