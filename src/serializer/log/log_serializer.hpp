// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef SERIALIZER_LOG_LOG_SERIALIZER_HPP_
#define SERIALIZER_LOG_LOG_SERIALIZER_HPP_

#include <map>
#include <string>
#include <vector>

#include "serializer/serializer.hpp"
#include "serializer/log/config.hpp"
#include "utils.hpp"
#include "concurrency/mutex.hpp"
#include "concurrency/mutex_assertion.hpp"

#include "serializer/log/metablock_manager.hpp"
#include "serializer/log/extent_manager.hpp"
#include "serializer/log/lba/lba_list.hpp"
#include "serializer/log/stats.hpp"

class cond_t;
class data_block_manager_t;
struct block_magic_t;
class io_backender_t;
class log_serializer_t;

namespace data_block_manager {
struct shutdown_callback_t {
    virtual void on_datablock_manager_shutdown() = 0;
    virtual ~shutdown_callback_t() {}
};

struct metablock_mixin_t {
    int64_t active_extent;
} __attribute__((__packed__));

}  // namespace data_block_manager

/**
 * This is the log-structured serializer, the holiest of holies of
 * RethinkDB. Please treat it with courtesy, professionalism, and
 * respect that it deserves.
 */

struct log_serializer_metablock_t {
    extent_manager_t::metablock_mixin_t extent_manager_part;
    lba_list_t::metablock_mixin_t lba_index_part;
    data_block_manager::metablock_mixin_t data_block_manager_part;
    block_sequence_id_t block_sequence_id;
} __attribute__((__packed__));

//  Data to be serialized to disk with each block.  Changing this changes the disk format!
// TODO: This header data should maybe go to the cache
typedef metablock_manager_t<log_serializer_metablock_t> mb_manager_t;

// Used to open a file (with the given filepath) for the log serializer.
class filepath_file_opener_t : public serializer_file_opener_t {
public:
    filepath_file_opener_t(const serializer_filepath_t &filepath,
                           io_backender_t *backender);
    ~filepath_file_opener_t();

    // The path of the final position of the file.
    std::string file_name() const;

    void open_serializer_file_create_temporary(scoped_ptr_t<file_t> *file_out);
    void move_serializer_file_to_permanent_location();
    void open_serializer_file_existing(scoped_ptr_t<file_t> *file_out);
    void unlink_serializer_file();
#ifdef SEMANTIC_SERIALIZER_CHECK
    void open_semantic_checking_file(scoped_ptr_t<semantic_checking_file_t> *file_out);
#endif

private:
    void open_serializer_file(const std::string &path, int extra_flags, scoped_ptr_t<file_t> *file_out);

    // The path of the temporary file.  This is file_name() with some suffix appended.
    std::string temporary_file_name() const;

    // Either file_name() or temporary_file_name(), depending on whether opened_temporary_ is true.
    std::string current_file_name() const;

    // The filepath of the final position of the file.
    const serializer_filepath_t filepath_;

    io_backender_t *const backender_;

    // Makes sure that only one member function gets called at a time.  Some of them are blocking,
    // and we don't want to have to worry about stuff like what the value of opened_temporary_
    // should be during the blocking call to move_serializer_file_to_permanent_location().
    mutex_assertion_t reentrance_mutex_;

    // This begins false.  It becomes true when open_serializer_file_create_temporary is called.  It
    // becomes false again when move_serializer_file_to_permanent_location is called.  It is used by
    // open_serializer_file_existing to know whether it should use the temporary or permanent path.
    bool opened_temporary_;

    DISABLE_COPYING(filepath_file_opener_t);
};


// Used internally
struct ls_start_existing_fsm_t;

class log_serializer_t :
#ifndef SEMANTIC_SERIALIZER_CHECK
    public serializer_t,
#else
    public home_thread_mixin_t,
#endif  // SEMANTIC_SERIALIZER_CHECK
    private data_block_manager::shutdown_callback_t,
    private lba_list_t::shutdown_callback_t
{
    friend struct ls_start_existing_fsm_t;
    friend class data_block_manager_t;
    friend class dbm_read_ahead_t;
    friend class ls_block_token_pointee_t;

public:
    /* Serializer configuration. dynamic_config_t is everything that can be changed from run
    to run; static_config_t is the parameters that are set when the database is created and
    cannot be changed after that. */
    typedef log_serializer_dynamic_config_t dynamic_config_t;
    typedef log_serializer_static_config_t static_config_t;

public:

    /* Blocks. Does not check for an existing database--use check_existing for that. */
    static void create(serializer_file_opener_t *file_opener, static_config_t static_config);

    /* Blocks. */
    log_serializer_t(dynamic_config_t dynamic_config, serializer_file_opener_t *file_opener, perfmon_collection_t *perfmon_collection);

    /* Blocks. */
    virtual ~log_serializer_t();

public:
    /* Implementation of the serializer_t API */
    scoped_malloc_t<ser_buffer_t> malloc();
    scoped_malloc_t<ser_buffer_t> clone(const ser_buffer_t *);

    file_account_t *make_io_account(int priority, int outstanding_requests_limit);

    void register_read_ahead_cb(serializer_read_ahead_callback_t *cb);
    void unregister_read_ahead_cb(serializer_read_ahead_callback_t *cb);
    block_id_t max_block_id();
    repli_timestamp_t get_recency(block_id_t id);

    bool get_delete_bit(block_id_t id);
    counted_t<ls_block_token_pointee_t> index_read(block_id_t block_id);

    void block_read(const counted_t<ls_block_token_pointee_t> &token, ser_buffer_t *buf, file_account_t *io_account);

    void index_write(const std::vector<index_write_op_t> &write_ops, file_account_t *io_account);

    std::vector<counted_t<ls_block_token_pointee_t> > block_writes(const std::vector<buf_write_info_t> &write_infos,
                                                                   file_account_t *io_account, iocallback_t *cb);

    block_size_t get_block_size() const;

    bool coop_lock_and_check();

private:
    void register_block_token(ls_block_token_pointee_t *token, int64_t offset);
    bool tokens_exist_for_offset(int64_t off);
    void unregister_block_token(ls_block_token_pointee_t *token);
    void remap_block_to_new_offset(int64_t current_offset, int64_t new_offset);
    counted_t<ls_block_token_pointee_t> generate_block_token(int64_t offset,
                                                             block_size_t block_size);

    void offer_buf_to_read_ahead_callbacks(
            block_id_t block_id,
            scoped_malloc_t<ser_buffer_t> &&buf,
            const counted_t<standard_block_token_t>& token,
            repli_timestamp_t recency_timestamp);
    bool should_perform_read_ahead();

    struct index_write_context_t {
        index_write_context_t() : next_metablock_write(NULL) { }
        extent_transaction_t extent_txn;
        cond_t *next_metablock_write;

    private:
        DISABLE_COPYING(index_write_context_t);
    };
    /* Starts a new transaction, updates perfmons etc. */
    void index_write_prepare(index_write_context_t *context, file_account_t *io_account);
    /* Finishes a write transaction */
    void index_write_finish(index_write_context_t *context, file_account_t *io_account);

    /* This mess is because the serializer is still mostly FSM-based */
    bool shutdown(cond_t *cb);
    bool next_shutdown_step();

    virtual void on_datablock_manager_shutdown();
    virtual void on_lba_shutdown();

    typedef log_serializer_metablock_t metablock_t;
    void prepare_metablock(metablock_t *mb_buffer);

    void consider_start_gc();

    std::multimap<int64_t, ls_block_token_pointee_t *> offset_tokens;
    scoped_ptr_t<log_serializer_stats_t> stats;
    perfmon_collection_t disk_stats_collection;
    perfmon_membership_t disk_stats_membership;

    // TODO: Just make this available in release mode?
#ifndef NDEBUG
    // Makes sure we get no tokens after we thought that
    bool expecting_no_more_tokens;
#endif

    std::vector<serializer_read_ahead_callback_t *> read_ahead_callbacks;

    const dynamic_config_t dynamic_config;
    static_config_t static_config;

    cond_t *shutdown_callback;

    enum shutdown_state_t {
        shutdown_begin,
        shutdown_waiting_on_serializer,
        shutdown_waiting_on_datablock_manager,
        shutdown_waiting_on_block_tokens,
        shutdown_waiting_on_lba
    } shutdown_state;
    bool shutdown_in_one_shot;

    enum state_t {
        state_unstarted,
        state_starting_up,
        state_ready,
        state_shutting_down,
        state_shut_down
    } state;

    file_t *dbfile;

    extent_manager_t *extent_manager;
    mb_manager_t *metablock_manager;
    lba_list_t *lba_index;
    data_block_manager_t *data_block_manager;

    /* The running index writes organize themselves into a list so that they can be sure to
    write their metablocks in the correct order. last_write points to the most recent
    transaction that started but did not finish; new index writes use it to find the
    end of the list so they can append themselves to it. */
    index_write_context_t *last_write;

    int active_write_count;

    block_sequence_id_t latest_block_sequence_id;

    DISABLE_COPYING(log_serializer_t);
};

#endif /* SERIALIZER_LOG_LOG_SERIALIZER_HPP_ */
