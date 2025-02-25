// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2021-2022, Intel Corporation */

/*
 * reserve_publish.cpp -- pmemstream_reserve and pmemstream_publish functional test.
 *			It checks if we can reserve space for entry, write to that space, and persist it.
 *			It's executed among "regular" appends to confirm we can mix these up.
 */

#include <cstring>
#include <vector>

#include <rapidcheck.h>

#include "libpmemstream_internal.h"
#include "stream_helpers.hpp"
#include "unittest.hpp"

int main(int argc, char *argv[])
{
	if (argc != 2) {
		UT_FATAL("usage: %s file-path", argv[0]);
	}

	auto path = std::string(argv[1]);

	return run_test([&] {
		return_check ret;

		ret += rc::check(
			"verify if mixing reserve+publish with append works fine",
			[&](const std::vector<std::string> &data, const std::vector<std::string> &extra_data,
			    const bool use_append, const bool is_runtime_initialized) {
				auto stream = make_pmemstream(path, TEST_DEFAULT_BLOCK_SIZE, TEST_DEFAULT_STREAM_SIZE);
				auto region =
					initialize_stream_single_region(stream.get(), TEST_DEFAULT_REGION_SIZE, data);
				verify(stream.get(), region, data, {});

				if (use_append) {
					append(stream.get(), region, nullptr, extra_data);
				} else {
					reserve_and_publish(stream.get(), region, is_runtime_initialized, extra_data);
				}

				/* add one more "regular" append */
				std::vector<std::string> my_data(extra_data);
				my_data.emplace_back(1024, 'Z');
				const auto extra_entry = my_data.back();
				int ret = pmemstream_append(stream.get(), region, nullptr, extra_entry.data(),
							    extra_entry.size(), nullptr);
				UT_ASSERTeq(ret, 0);
				verify(stream.get(), region, data, my_data);

				UT_ASSERTeq(pmemstream_region_free(stream.get(), region), 0);
			});

		ret += rc::check(
			"verify if reserve+publish by hand will behave the same as regular append",
			[&](const std::vector<std::string> &data, const bool is_runtime_initialized) {
				/* regular append of 'data' */
				std::vector<std::string> a_data;
				{
					auto stream = make_pmemstream(path, TEST_DEFAULT_BLOCK_SIZE,
								      TEST_DEFAULT_STREAM_SIZE);
					auto region = initialize_stream_single_region(stream.get(),
										      TEST_DEFAULT_REGION_SIZE, data);
					verify(stream.get(), region, data, {});
					a_data = get_elements_in_region(stream.get(), region);

					UT_ASSERTeq(pmemstream_region_free(stream.get(), region), 0);
				}
				/* publish-reserve by hand of the same 'data' (in a different file) */
				std::vector<std::string> rp_data;
				{
					auto stream = make_pmemstream(path + "_2", TEST_DEFAULT_BLOCK_SIZE,
								      TEST_DEFAULT_STREAM_SIZE);
					auto region = initialize_stream_single_region(stream.get(),
										      TEST_DEFAULT_REGION_SIZE, {});
					reserve_and_publish(stream.get(), region, is_runtime_initialized, data);
					rp_data = get_elements_in_region(stream.get(), region);

					UT_ASSERT(std::equal(a_data.begin(), a_data.end(), rp_data.begin(),
							     rp_data.end()));

					UT_ASSERTeq(pmemstream_region_free(stream.get(), region), 0);
				}
			});

		ret += rc::check("verify if not calling publish does not result in data being visible",
				 [&](const std::vector<std::string> &data, const std::string &extra_entry) {
					 pmemstream_region region;
					 {
						 auto stream = make_pmemstream(path, TEST_DEFAULT_BLOCK_SIZE,
									       TEST_DEFAULT_STREAM_SIZE);
						 region = initialize_stream_single_region(
							 stream.get(), TEST_DEFAULT_REGION_SIZE, data);

						 void *reserved_data;
						 pmemstream_entry reserved_entry;
						 int ret = pmemstream_reserve(stream.get(), region, nullptr,
									      extra_entry.size(), &reserved_entry,
									      &reserved_data);
						 UT_ASSERTeq(ret, 0);

						 std::memcpy(reinterpret_cast<char *>(reserved_data),
							     extra_entry.data(), extra_entry.size());
						 stream->data.persist(reserved_data, extra_entry.size());
						 verify(stream.get(), region, data, {});
					 }
					 {
						 auto stream = make_pmemstream(path, TEST_DEFAULT_BLOCK_SIZE,
									       TEST_DEFAULT_STREAM_SIZE, false);
						 verify(stream.get(), region, data, {});
					 }
				 });
	});
}
