/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2021 ScyllaDB
 */
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/util/closeable.hh>
#include <yaml-cpp/yaml.h>

using namespace seastar;

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("directory", bpo::value<sstring>()->default_value("."), "directory to work on")
    ;

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            auto& opts = app.configuration();
            auto& storage = opts["directory"].as<sstring>();

            YAML::Emitter out;
            out << YAML::BeginDoc;
            out << YAML::BeginMap;

            engine().open_file_dma(storage + "/tempfile", open_flags::rw | open_flags::create | open_flags::exclusive).then([&] (file f) {
                return with_closeable(std::move(f), [&out, &storage] (file& f) {
                    return remove_file(storage + "/tempfile").then([&out, &f] {
                        out << YAML::Key << "disk_read_max_length" << YAML::Value << f.disk_read_max_length();
                        out << YAML::Key << "disk_write_max_length" << YAML::Value << f.disk_write_max_length();
                    }).then([&out, &f] {
                        return f.stat().then([&out] (auto st) {
                            auto& ioq = engine().get_io_queue(st.st_dev);
                            auto& cfg = ioq.get_config();

                            out << YAML::Key << "io_queue" << YAML::BeginMap;
                            out << YAML::Key << "req_count_rate" << YAML::Value << cfg.req_count_rate;
                            out << YAML::Key << "blocks_count_rate" << YAML::Value << cfg.blocks_count_rate;
                            out << YAML::Key << "disk_req_write_to_read_multiplier" << YAML::Value << cfg.disk_req_write_to_read_multiplier;
                            out << YAML::Key << "disk_blocks_write_to_read_multiplier" << YAML::Value << cfg.disk_blocks_write_to_read_multiplier;
                            out << YAML::EndMap;

                            out << YAML::Key << "fair_queue" << YAML::BeginMap;
                            out << YAML::Key << "capacities" << YAML::BeginMap;
                            auto request_io_capacity = [&ioq] (internal::io_direction_and_length dnl) {
                                auto stream = ioq.request_stream(dnl);
                                auto ticket = internal::make_ticket(dnl, ioq.get_config());
                                return internal::get_fair_group(ioq, stream).ticket_capacity(ticket);
                            };
                            for (size_t sz = 512; sz <= 128 * 1024; sz <<= 1) {
                                out << YAML::Key << sz << YAML::BeginMap;
                                out << YAML::Key << "read" << YAML::Value << request_io_capacity(internal::io_direction_and_length(internal::io_direction_and_length::read_idx, sz));
                                out << YAML::Key << "write" << YAML::Value << request_io_capacity(internal::io_direction_and_length(internal::io_direction_and_length::write_idx, sz));
                                out << YAML::EndMap;
                            }
                            out << YAML::EndMap;

                            const auto& fg = internal::get_fair_group(ioq, internal::io_direction_and_length::write_idx);
                            out << YAML::Key << "cost_capacity" << YAML::Value << format("{}", fg.cost_capacity());
                            out << YAML::Key << "per_tick_grab_threshold" << YAML::Value << fg.per_tick_grab_threshold();

                            const auto& tb = fg.token_bucket();
                            out << YAML::Key << "token_bucket" << YAML::BeginMap;
                            out << YAML::Key << "limit" << YAML::Value << tb.limit();
                            out << YAML::Key << "rate" << YAML::Value << tb.rate();
                            out << YAML::Key << "threshold" << YAML::Value << tb.threshold();
                            out << YAML::EndMap;

                            out << YAML::EndMap;
                        });
                    });
                });
            }).get();

            out << YAML::EndMap;
            out << YAML::EndDoc;
            std::cout << out.c_str();
        });
    });
}
