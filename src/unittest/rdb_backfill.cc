// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "clustering/administration/metadata.hpp"
#include "clustering/immediate_consistency/branch/backfill_throttler.hpp"
#include "clustering/immediate_consistency/branch/broadcaster.hpp"
#include "clustering/immediate_consistency/branch/listener.hpp"
#include "clustering/immediate_consistency/branch/replier.hpp"
#include "extproc/extproc_pool.hpp"
#include "extproc/extproc_spawner.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/pb_utils.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/store.hpp"
#include "rdb_protocol/sym.hpp"
#include "rpc/directory/read_manager.hpp"
#include "rpc/semilattice/semilattice_manager.hpp"
#include "stl_utils.hpp"
#include "unittest/branch_history_manager.hpp"
#include "unittest/clustering_utils.hpp"
#include "unittest/dummy_metadata_controller.hpp"
#include "unittest/unittest_utils.hpp"

namespace unittest {

void run_with_broadcaster(
    std::function< void(
        std::pair<io_backender_t *, simple_mailbox_cluster_t *>,
        branch_history_manager_t *,
        scoped_ptr_t<broadcaster_t> *,
        test_store_t *,
        scoped_ptr_t<listener_t> *,
        rdb_context_t *ctx,
        order_source_t *)> fun) {
    order_source_t order_source;

    /* Set up a cluster so mailboxes can be created */
    simple_mailbox_cluster_t cluster;

    /* Set up branch history manager */
    in_memory_branch_history_manager_t branch_history_manager;

    // io backender
    io_backender_t io_backender(file_direct_io_mode_t::buffered_desired);

    extproc_pool_t extproc_pool(2);
    rdb_context_t ctx(&extproc_pool, NULL);

    /* Set up a broadcaster and initial listener */
    test_store_t initial_store(&io_backender, &order_source, &ctx);

    cond_t interruptor;

    branch_birth_certificate_t branch_info;
    branch_info.region = region_t::universe();
    branch_info.origin =
        region_map_t<version_t>(region_t::universe(), version_t::zero());
    branch_info.initial_timestamp = state_timestamp_t::zero();

    scoped_ptr_t<broadcaster_t> broadcaster(
        new broadcaster_t(
            cluster.get_mailbox_manager(),
            &branch_history_manager,
            &initial_store.store,
            &get_global_perfmon_collection(),
            generate_uuid(),
            branch_info,
            &order_source,
            &interruptor));

    scoped_ptr_t<listener_t> initial_listener(new listener_t(
        base_path_t("."), //TODO is it bad that this isn't configurable?
        &io_backender,
        cluster.get_mailbox_manager(),
        generate_uuid(),
        broadcaster.get(),
        &get_global_perfmon_collection(),
        &interruptor,
        &order_source));

    fun(std::make_pair(&io_backender, &cluster),
        &branch_history_manager,
        &broadcaster,
        &initial_store,
        &initial_listener,
        &ctx,
        &order_source);
}

void run_in_thread_pool_with_broadcaster(
        std::function< void(std::pair<io_backender_t *, simple_mailbox_cluster_t *>,
                            branch_history_manager_t *,
                            scoped_ptr_t<broadcaster_t> *,
                            test_store_t *,
                            scoped_ptr_t<listener_t> *,
                            rdb_context_t *,
                            order_source_t *)> fun)
{
    extproc_spawner_t extproc_spawner;
    run_in_thread_pool(std::bind(&run_with_broadcaster, fun));
}


/* `PartialBackfill` backfills only in a specific sub-region. */

ql::datum_t generate_document(size_t value_padding_length, const std::string &value) {
    ql::configured_limits_t limits;
    // This is a kind of hacky way to add an object to a map but I'm not sure
    // anyone really cares.
    return ql::to_datum(scoped_cJSON_t(cJSON_Parse(strprintf("{\"id\" : %s, \"padding\" : \"%s\"}",
                                                             value.c_str(),
                                                             std::string(value_padding_length, 'a').c_str()).c_str())).get(),
                        limits, reql_version_t::LATEST);
}

void write_to_broadcaster(size_t value_padding_length,
                          broadcaster_t *broadcaster,
                          const std::string &key,
                          const std::string &value,
                          order_token_t otok,
                          signal_t *) {
    write_t write(
            point_write_t(
                store_key_t(key),
                generate_document(value_padding_length, value),
                true),
            DURABILITY_REQUIREMENT_DEFAULT,
            profile_bool_t::PROFILE,
            ql::configured_limits_t());
    simple_write_callback_t write_callback;
    broadcaster->spawn_write(write, otok, &write_callback);
    write_callback.wait_lazily_unordered();
}

void run_backfill_test(size_t value_padding_length,
                       std::pair<io_backender_t *, simple_mailbox_cluster_t *> io_backender_and_cluster,
                       branch_history_manager_t *branch_history_manager,
                       scoped_ptr_t<broadcaster_t> *broadcaster,
                       test_store_t *,
                       scoped_ptr_t<listener_t> *initial_listener,
                       rdb_context_t *ctx,
                       order_source_t *order_source) {
    io_backender_t *const io_backender = io_backender_and_cluster.first;
    simple_mailbox_cluster_t *const cluster = io_backender_and_cluster.second;

    recreate_temporary_directory(base_path_t("."));
    /* Set up a replier so the broadcaster can handle operations */
    replier_t replier(initial_listener->get(), cluster->get_mailbox_manager(), branch_history_manager);

    /* Start sending operations to the broadcaster */
    std::map<std::string, std::string> inserter_state;
    test_inserter_t inserter(
        std::bind(&write_to_broadcaster, value_padding_length, broadcaster->get(),
                  ph::_1, ph::_2, ph::_3, ph::_4),
        std::function<std::string(const std::string &, order_token_t, signal_t *)>(),
        &mc_key_gen,
        order_source,
        "rdb_backfill run_partial_backfill_test inserter",
        &inserter_state);
    nap(10000);

    backfill_throttler_t backfill_throttler;

    /* Set up a second mirror */
    test_store_t store2(io_backender, order_source, ctx);
    cond_t interruptor;
    listener_t listener2(
        base_path_t("."),
        io_backender,
        cluster->get_mailbox_manager(),
        generate_uuid(),
        &backfill_throttler,
        (*broadcaster)->get_business_card(),
        branch_history_manager,
        &store2.store,
        replier.get_business_card(),
        &get_global_perfmon_collection(),
        &interruptor,
        order_source,
        nullptr);

    nap(10000);

    /* Stop the inserter, then let any lingering writes finish */
    inserter.stop();
    /* Let any lingering writes finish */
    // TODO: 100 seconds?
    nap(100000);

    for (std::map<std::string, std::string>::iterator it = inserter_state.begin();
            it != inserter_state.end(); it++) {
        read_t read(point_read_t(store_key_t(it->first)), profile_bool_t::PROFILE);
        fifo_enforcer_source_t fifo_source;
        fifo_enforcer_sink_t fifo_sink;
        fifo_enforcer_sink_t::exit_read_t exiter(&fifo_sink, fifo_source.enter_read());
        cond_t non_interruptor;
        read_response_t response;
        broadcaster->get()->read(read, &response, &exiter, order_source->check_in("unittest::(rdb)run_partial_backfill_test").with_read_mode(), &non_interruptor);
        point_read_response_t get_result = boost::get<point_read_response_t>(response.response);
        EXPECT_TRUE(get_result.data.has());
        EXPECT_EQ(generate_document(value_padding_length,
                                     it->second),
                  get_result.data);
    }
}

TEST(RDBProtocolBackfill, Backfill) {
     run_in_thread_pool_with_broadcaster(
         std::bind(&run_backfill_test, 0, ph::_1, ph::_2, ph::_3, ph::_4, ph::_5,
            ph::_6, ph::_7));
}

TEST(RDBProtocolBackfill, BackfillLargeValues) {
     run_in_thread_pool_with_broadcaster(
         std::bind(&run_backfill_test, 300, ph::_1, ph::_2, ph::_3, ph::_4, ph::_5,
            ph::_6, ph::_7));
}

void run_sindex_backfill_test(std::pair<io_backender_t *, simple_mailbox_cluster_t *> io_backender_and_cluster,
                              branch_history_manager_t *branch_history_manager,
                              scoped_ptr_t<broadcaster_t> *broadcaster,
                              test_store_t *,
                              scoped_ptr_t<listener_t> *initial_listener,
                              rdb_context_t *ctx,
                              order_source_t *order_source) {
    io_backender_t *const io_backender = io_backender_and_cluster.first;
    backfill_throttler_t backfill_throttler;
    simple_mailbox_cluster_t *const cluster = io_backender_and_cluster.second;

    recreate_temporary_directory(base_path_t("."));
    /* Set up a replier so the broadcaster can handle operations */
    
    replier_t replier(initial_listener->get(), cluster->get_mailbox_manager(), branch_history_manager);
    nap(100);   /* make time for the broadcaster to find out about the replier */

    std::string id("sid");
    {
        /* Create a secondary index object. */
        const ql::sym_t one(1);
        ql::protob_t<const Term> mapping = ql::r::var(one)["id"].release_counted();
        ql::map_wire_func_t m(mapping, make_vector(one), get_backtrace(mapping));

        write_t write(sindex_create_t(id, m, sindex_multi_bool_t::SINGLE,
                                      sindex_geo_bool_t::REGULAR),
                      profile_bool_t::PROFILE, ql::configured_limits_t());

        simple_write_callback_t write_callback;
        broadcaster->get()->spawn_write(write, order_token_t::ignore, &write_callback);
        write_callback.wait_lazily_unordered();
    }

    /* Start sending operations to the broadcaster */
    std::map<std::string, std::string> inserter_state;
    test_inserter_t inserter(
        std::bind(&write_to_broadcaster, 0, broadcaster->get(),
                  ph::_1, ph::_2, ph::_3, ph::_4),
        std::function<std::string(const std::string &, order_token_t, signal_t *)>(),
        &mc_key_gen,
        order_source,
        "rdb_backfill run_partial_backfill_test inserter",
        &inserter_state);
    nap(10000);

    /* Set up a second mirror */
    test_store_t store2(io_backender, order_source, ctx);
    cond_t interruptor;
    listener_t listener2(
        base_path_t("."),
        io_backender,
        cluster->get_mailbox_manager(),
        generate_uuid(),
        &backfill_throttler,
        (*broadcaster)->get_business_card(),
        branch_history_manager,
        &store2.store,
        replier.get_business_card(),
        &get_global_perfmon_collection(),
        &interruptor,
        order_source,
        nullptr);

    nap(10000);

    /* Stop the inserter, then let any lingering writes finish */
    inserter.stop();
    /* Let any lingering writes finish */
    // TODO: 100 seconds?
    nap(100000);

    for (std::map<std::string, std::string>::iterator it = inserter_state.begin();
            it != inserter_state.end(); it++) {
        scoped_cJSON_t sindex_key_json(cJSON_Parse(it->second.c_str()));
        auto sindex_key_literal = ql::to_datum(sindex_key_json.get(),
                                               ql::configured_limits_t(),
                                               reql_version_t::LATEST);
        read_t read = make_sindex_read(sindex_key_literal, id);
        fifo_enforcer_source_t fifo_source;
        fifo_enforcer_sink_t fifo_sink;
        fifo_enforcer_sink_t::exit_read_t exiter(&fifo_sink, fifo_source.enter_read());
        cond_t non_interruptor;
        read_response_t response;
        broadcaster->get()->read(read, &response, &exiter, order_source->check_in("unittest::(rdb)run_partial_backfill_test").with_read_mode(), &non_interruptor);
        rget_read_response_t get_result = boost::get<rget_read_response_t>(response.response);
        auto groups = boost::get<ql::grouped_t<ql::stream_t> >(&get_result.result);
        ASSERT_TRUE(groups != NULL);
        ASSERT_EQ(1, groups->size());
        // Order doesn't matter because groups->size() is 1.
        auto result_stream = &groups->begin(ql::grouped::order_doesnt_matter_t())->second;
        ASSERT_EQ(1u, result_stream->size());
        EXPECT_EQ(generate_document(0, it->second), result_stream->at(0).data);
    }
}

TEST(RDBProtocolBackfill, SindexBackfill) {
     run_in_thread_pool_with_broadcaster(&run_sindex_backfill_test);
}

}   /* namespace unittest */
