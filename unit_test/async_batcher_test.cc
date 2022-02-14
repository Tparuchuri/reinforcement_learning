#define BOOST_TEST_DYN_LINK
#ifdef STAND_ALONE
#   define BOOST_TEST_MODULE Main
#endif
#include <boost/test/unit_test.hpp>
#include <string>
#include <vector>
#include "data_buffer.h"
#include "err_constants.h"
#include "serialization/json_serializer.h"
#include "logger/async_batcher.h"
#include "sender.h"
#include "vw_math.h"
#include "config_utility.h"
#include <iostream>

using namespace reinforcement_learning;
//This class simply implement a 'send' method, in order to be used as a template in the async_batcher
class message_sender_without_event_counts : public logger::i_message_sender {
  std::vector<std::string>& items;
public:
  explicit message_sender_without_event_counts(std::vector<std::string>& _items)
    : items(_items), sender{nullptr} {}

  int send(const uint16_t msg_type, const buffer& db, api_status* status = nullptr) override {
    items.emplace_back(reinterpret_cast<char*>(db->body_begin()));
    return error_code::success;
  };
  int init(api_status* status) override { return error_code::success; };
  i_sender* sender;
};

class message_sender : public logger::i_message_sender {
public:
    std::vector<int>& events_count;
    std::vector<std::string>& items;
    explicit message_sender(std::vector<std::string>& _items, std::vector<int>& _events_count)
        : events_count(_events_count), items(_items), sender{ nullptr } {}
    int send(const uint16_t msg_type, const buffer& db, api_status* status = nullptr) override {
        items.emplace_back(reinterpret_cast<char*>(db->body_begin()));
        return error_code::success;
    };
    int send(const uint16_t msg_type, const buffer& db, unsigned int original_events,api_status* status = nullptr) override {
        events_count.emplace_back(original_events);
        return send(msg_type, db, status);
    };
    int init(api_status* status) override { return error_code::success; };
    i_sender* sender;
};

class test_undroppable_event : public event {
public:
  test_undroppable_event() {}

  test_undroppable_event(const std::string& id)
    : event(id.c_str(), timestamp{}) {}

  test_undroppable_event(test_undroppable_event&& other) = default;

  test_undroppable_event& operator=(test_undroppable_event&& other) = default;

  bool try_drop(float drop_prob, int _drop_pass) override { return false; }
  std::string get_event_id() { return _seed_id; }
};

class test_droppable_event : public event {
public:
  test_droppable_event() {}

  test_droppable_event(const std::string& id)
    : event(id.c_str(), timestamp{}) {}

  test_droppable_event(test_droppable_event&& other) = default;

  test_droppable_event& operator=(test_droppable_event&& other) = default;

  bool try_drop(float drop_prob, int _drop_pass) override { return true; }
};

// event that converts the event_id into a float used for the drop probability
class config_drop_event : public event {
public:
  config_drop_event() {}
  config_drop_event(const std::string& id) : event(id.c_str(), timestamp{}) {}

  config_drop_event(config_drop_event&& other) = default;
  config_drop_event& operator=(config_drop_event&& other) = default;

  bool try_drop(float drop_prob, int _drop_pass) override {
    auto prob = static_cast<float>(std::atof(_seed_id.c_str()));

    // logic because floating point comparison is dumb. In this case, atof(0.7) > 0.7 == true
    return (prob > drop_prob) && !VW::math::are_same(prob, drop_prob);
  }
  std::string get_event_id() { return _seed_id; }
};

namespace reinforcement_learning { namespace logger {
  template <>
  struct json_event_serializer<test_droppable_event> {
    using serializer_t = json_event_serializer<test_droppable_event>;

    static int serialize(test_droppable_event& evt, std::ostream& out, api_status* status) {
      out << evt.get_seed_id();
      return error_code::success;
    }

    static size_t size_estimate(const test_droppable_event& evt) { return 1; }
  };
  template <>
  struct json_event_serializer<test_undroppable_event> {
    using serializer_t = json_event_serializer<test_undroppable_event>;

    static int serialize(test_undroppable_event& evt, std::ostream& out, api_status* status) {
      out << evt.get_event_id();
      return error_code::success;
    }

    static size_t size_estimate(const test_undroppable_event& evt) { return 1; }
  };
    
  template <>
  struct json_event_serializer<config_drop_event> {
    using serializer_t = json_event_serializer<config_drop_event>;

    static int serialize(config_drop_event& evt, std::ostream& out, api_status* status) {
      out << evt.get_event_id();
      return error_code::success;
    }

    static size_t size_estimate(const config_drop_event& evt) { return 1; }
  };
}}

void expect_no_error(const api_status& s, void* cntxt) {
  BOOST_ASSERT(s.get_error_code() == error_code::success);
  BOOST_FAIL("Should not get background error notifications");
}

//test the flush mechanism based on a timer
BOOST_AUTO_TEST_CASE(flush_timeout) {
  std::vector<std::string> items;
  auto s = new message_sender_without_event_counts(items);
  size_t timeout_ms = 100; //set a short timeout
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = 262143;
  config.send_batch_interval_ms = timeout_ms;
  config.send_queue_max_capacity = 8192;
  int dummy = 0;
  logger::async_batcher<test_undroppable_event> batcher(s, watchdog, dummy, &error_fn, config);
  batcher.init(nullptr); // Allow periodic_background_proc inside async_batcher to start waiting
  // on a timer before sending any events to it. Else we risk not
  // triggering the batch mechanism and might get triggered by initial
  // pass in do..while loop
  std::this_thread::sleep_for(std::chrono::milliseconds(20)); //add 2 items in the current batch
  std::string foo("foo");
  std::string bar("bar");
  batcher.append(test_undroppable_event(foo));
  batcher.append(test_undroppable_event(bar)); //check the batch was sent
  std::string expected = foo + "\n" + bar + "\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  BOOST_REQUIRE_EQUAL(items.size(), 1);
  std::string result;
  for (auto item : items) { result.append(item.begin(), item.end()); }
  BOOST_CHECK_EQUAL(result, expected);
}

BOOST_AUTO_TEST_CASE(flush_timeout_with_counter_ENABLE) {
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  size_t timeout_ms = 100; //set a short timeout
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = 262143;
  config.send_batch_interval_ms = timeout_ms;
  config.send_queue_max_capacity = 8192;
  config.events_counter_status = events_counter_status::ENABLE;
  config.subsample_rate = 0.7f;

  int dummy = 0;
  logger::async_batcher<test_undroppable_event> batcher(s, watchdog, dummy, &error_fn, config);
  batcher.init(nullptr); // Allow periodic_background_proc inside async_batcher to start waiting
  // on a timer before sending any events to it. Else we risk not
  // triggering the batch mechanism and might get triggered by initial
  // pass in do..while loop
  std::this_thread::sleep_for(std::chrono::milliseconds(20)); //add 2 items in the current batch
  std::string foo("foo");
  std::string bar("bar");
  batcher.append(test_undroppable_event(foo));
  batcher.append(test_undroppable_event(bar)); //check the batch was sent
  std::string expected = foo + "\n" + bar + "\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  BOOST_REQUIRE_EQUAL(items.size(), 1);
  BOOST_REQUIRE_EQUAL(events_count.size(), 1);
  std::string result;
  for (auto item : items) { result.append(item.begin(), item.end()); }
  BOOST_CHECK_EQUAL(result, expected);
  BOOST_CHECK_EQUAL(events_count[0], 2);
}

//test that the batcher split batches as expected
BOOST_AUTO_TEST_CASE(flush_batches) {
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  size_t send_high_water_mark = 10; //bytes
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = send_high_water_mark;
  config.send_batch_interval_ms = 100000;
  
  int dummy = 0;
  auto batcher = new logger::async_batcher<test_undroppable_event>
      (s, watchdog, dummy, &error_fn, config);
  batcher->init(nullptr); // Allow periodic_background_proc inside async_batcher to start waiting
  // on a timer before sending any events to it.   Else we risk not
  // triggering the batch mechanism and might get triggered by initial
  // pass in do..while loop
  std::this_thread::sleep_for(std::chrono::milliseconds(20)); //add 2 items in the current batch
  std::string foo("foo");
  std::string bar("bar-yyy");
  batcher->append(test_undroppable_event(foo)); //3 bytes
  batcher->append(test_undroppable_event(bar)); //7 bytes
  //'send_high_water_mark' will be triggered by previous 2 items.
  //next item will be added in a new batch
  std::string hello("hello");
  batcher->append(test_undroppable_event(hello));
  const std::string expected_batch_0 = foo + "\n" + bar + "\n";
  const std::string expected_batch_1 = hello + "\n";
  delete batcher; //flush force
  BOOST_REQUIRE_EQUAL(items.size(), 2);
  BOOST_CHECK_EQUAL(items[0], expected_batch_0);
  BOOST_CHECK_EQUAL(items[1], expected_batch_1);
  BOOST_REQUIRE(events_count.empty());
}

BOOST_AUTO_TEST_CASE(flush_batches_with_counter_ENABLE) {
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  size_t send_high_water_mark = 10; //bytes
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = send_high_water_mark;
  config.send_batch_interval_ms = 100000;
  config.events_counter_status = events_counter_status::ENABLE;
  config.subsample_rate = 0.7f;

  int dummy = 0;
  auto batcher = new logger::async_batcher<test_undroppable_event>
    (s, watchdog, dummy, &error_fn, config);
  batcher->init(nullptr); // Allow periodic_background_proc inside async_batcher to start waiting
  // on a timer before sending any events to it.   Else we risk not
  // triggering the batch mechanism and might get triggered by initial
  // pass in do..while loop
  std::this_thread::sleep_for(std::chrono::milliseconds(20)); //add 2 items in the current batch
  std::string foo("foo");
  std::string bar("bar-yyy");
  batcher->append(test_undroppable_event(foo)); //3 bytes
  batcher->append(test_undroppable_event(bar)); //7 bytes
  //'send_high_water_mark' will be triggered by previous 2 items.
  //next item will be added in a new batch
  std::string hello("hello");
  batcher->append(test_undroppable_event(hello));
  const std::string expected_batch_0 = foo + "\n" + bar + "\n";
  const std::string expected_batch_1 = hello + "\n";
  delete batcher; //flush force
  BOOST_REQUIRE_EQUAL(items.size(), 2);
  BOOST_REQUIRE_EQUAL(events_count.size(), 2);
  BOOST_CHECK_EQUAL(items[0], expected_batch_0);
  BOOST_CHECK_EQUAL(items[1], expected_batch_1);
  BOOST_CHECK_EQUAL(events_count[0], 2);
  BOOST_CHECK_EQUAL(events_count[1], 1);
}

//test that the batcher flushes everything before deletion
BOOST_AUTO_TEST_CASE(flush_after_deletion) {
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  int dummy = 0;
  auto* batcher = new logger::async_batcher<test_undroppable_event>
      (s, watchdog, dummy, nullptr, config);
  batcher->init(nullptr); // Allow periodic_background_proc to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::string foo("foo");
  std::string bar("bar");
  batcher->append(test_undroppable_event(foo));
  batcher->append(test_undroppable_event(bar)); //batch was not sent yet
  BOOST_CHECK_EQUAL(items.size(), 0);           //batch flush is triggered on delete
  delete batcher;                               //check the batch was sent
  BOOST_REQUIRE_EQUAL(items.size(), 1);
  std::string expected = foo + "\n" + bar + "\n";
  BOOST_CHECK_EQUAL(items[0], expected);
}

BOOST_AUTO_TEST_CASE(flush_after_deletion_with_counter_ENABLE) {
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.events_counter_status = events_counter_status::ENABLE;
  config.subsample_rate = 0.7f;

  int dummy = 0;
  auto* batcher = new logger::async_batcher<test_undroppable_event>
    (s, watchdog, dummy, nullptr, config);
  batcher->init(nullptr); // Allow periodic_background_proc to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::string foo("foo");
  std::string bar("bar");
  batcher->append(test_undroppable_event(foo));
  batcher->append(test_undroppable_event(bar)); //batch was not sent yet
  BOOST_CHECK_EQUAL(items.size(), 0);           //batch flush is triggered on delete
  BOOST_CHECK_EQUAL(events_count.size(), 0);    
  delete batcher;                               //check the batch was sent
  BOOST_REQUIRE_EQUAL(items.size(), 1);
  BOOST_REQUIRE_EQUAL(events_count.size(), 1);
  std::string expected = foo + "\n" + bar + "\n";
  BOOST_CHECK_EQUAL(items[0], expected);
  BOOST_CHECK_EQUAL(events_count[0], 2);
}

//test that events are not dropped using the queue_dropping_disable option, even if the queue max capacity is reached
BOOST_AUTO_TEST_CASE(queue_overflow_do_not_drop_event) {
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  size_t timeout_ms = 100;
  size_t queue_max_size = 3;
  queue_mode_enum queue_mode = queue_mode_enum::BLOCK;
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = 262143;
  config.send_batch_interval_ms = timeout_ms;
  config.send_queue_max_capacity = queue_max_size;
  config.queue_mode = queue_mode;
  int dummy = 0;
  auto batcher = new logger::async_batcher<test_droppable_event>(s, watchdog, dummy, &error_fn, config);
  batcher->init(nullptr); // Allow periodic_background_proc to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  int n = 10;
  for (int i = 0; i < n; ++i) { batcher->append(test_droppable_event(std::to_string(i))); } //triggers a final flush
  delete batcher;
  //all batches were sent. Check that no event was dropped
  std::string expected_output = "0\n";
  for (int i = 1; i < n; ++i) {
    expected_output += std::to_string(i);
    expected_output.append("\n");
  }
  BOOST_REQUIRE(!items.empty());
  std::string actual_output;
  for (const auto& item : items) { actual_output.append(item); }
  BOOST_CHECK_EQUAL(expected_output, actual_output);
}

//test that events are not dropped using the queue_dropping_disable option, even if the queue max capacity is reached
BOOST_AUTO_TEST_CASE(queue_overflow_do_not_drop_event_with_counter_ENABLE) {
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  size_t timeout_ms = 100;
  size_t queue_max_size = 3;
  queue_mode_enum queue_mode = queue_mode_enum::BLOCK;
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = 262143;
  config.send_batch_interval_ms = timeout_ms;
  config.send_queue_max_capacity = queue_max_size;
  config.queue_mode = queue_mode;
  config.events_counter_status = events_counter_status::ENABLE;
  config.subsample_rate = 0.7f;
  int dummy = 0;
  auto batcher = new logger::async_batcher<config_drop_event>(s, watchdog, dummy, &error_fn, config);
  batcher->init(nullptr); // Allow periodic_background_proc to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  int n = 10;
  for (int i = 0; i < n; ++i) { batcher->append(config_drop_event(std::to_string(i/10.0f))); } //triggers a final flush
  delete batcher;
  //all batches were sent. Check that no event was dropped
  std::string expected_output = "";
  for (int i = 0; i < 8; ++i) {
    expected_output += std::to_string(i/10.0f);
    expected_output.append("\n");
  }
  BOOST_REQUIRE(!items.empty());
  BOOST_REQUIRE(!events_count.empty());
  std::string actual_output;
  for (const auto& item : items) { actual_output.append(item); }
  unsigned int count = 0;
  for (const auto& event_count : events_count) { count+= event_count; }
  BOOST_CHECK_EQUAL(expected_output, actual_output);
  BOOST_CHECK_EQUAL(10, count);
}

BOOST_AUTO_TEST_CASE(queue_config_drop_rate_test)
{
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  size_t timeout_ms = 100;
  size_t queue_max_size = 10;
  queue_mode_enum queue_mode = queue_mode_enum::BLOCK;
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = 262143;
  config.send_batch_interval_ms = timeout_ms;
  config.send_queue_max_capacity = queue_max_size;
  config.queue_mode = queue_mode;
  config.subsample_rate = 0.7f;
  int dummy = 0;
  auto batcher = new logger::async_batcher<config_drop_event>(s, watchdog, dummy, &error_fn, config);
  batcher->init(nullptr);
  // Allow periodic_background_proc to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  batcher->append(config_drop_event("0.00"));
  batcher->append(config_drop_event("1.00"));
  batcher->append(config_drop_event("0.69"));
  batcher->append(config_drop_event("0.70"));
  batcher->append(config_drop_event("0.71"));

  delete batcher;

  BOOST_REQUIRE(!items.empty());
  BOOST_CHECK_EQUAL(items[0], "0.00\n0.69\n0.70\n");
}

BOOST_AUTO_TEST_CASE(queue_config_drop_rate_test_with_counter_ENABLE)
{
  std::vector<std::string> items;
  std::vector<int> events_count;
  auto s = new message_sender(items, events_count);
  size_t timeout_ms = 100;
  size_t queue_max_size = 10;
  queue_mode_enum queue_mode = queue_mode_enum::DROP;
  error_callback_fn error_fn(expect_no_error, nullptr);
  utility::watchdog watchdog(nullptr);
  utility::async_batcher_config config;
  config.send_high_water_mark = 262143;
  config.send_batch_interval_ms = timeout_ms;
  config.send_queue_max_capacity = queue_max_size;
  config.queue_mode = queue_mode;
  config.events_counter_status = events_counter_status::ENABLE;
  config.subsample_rate = 0.7f;
  int dummy = 0;
  auto batcher = new logger::async_batcher<config_drop_event>(s, watchdog, dummy, &error_fn, config);
  batcher->init(nullptr);
  // Allow periodic_background_proc to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  batcher->append(config_drop_event("0.00"));
  batcher->append(config_drop_event("1.00"));
  batcher->append(config_drop_event("0.69"));
  batcher->append(config_drop_event("0.70"));
  batcher->append(config_drop_event("0.71"));

  delete batcher;

  BOOST_REQUIRE(!items.empty());
  BOOST_REQUIRE(!events_count.empty());
  BOOST_CHECK_EQUAL(items[0], "0.00\n0.69\n0.70\n");
  BOOST_CHECK_EQUAL(events_count[0], 5);
}

BOOST_AUTO_TEST_CASE(get_batcher_config_counter_status_test)
{
  utility::configuration config;
  utility::async_batcher_config batcher_config = utility::get_batcher_config(config, INTERACTION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::DISABLE);
  config.set("interaction.sender.implementation", "INTERACTION_HTTP_API_SENDER");
  batcher_config = utility::get_batcher_config(config, INTERACTION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::DISABLE);
  config.set("protocol.version","2");
  batcher_config = utility::get_batcher_config(config, INTERACTION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::ENABLE);
  config.set("interaction.sender.implementation", "INTERACTION_FILE_SENDER");
  batcher_config = utility::get_batcher_config(config, INTERACTION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::DISABLE);
  config.set("interaction.sender.implementation", "INTERACTION_EH_SENDER");
  batcher_config = utility::get_batcher_config(config, INTERACTION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::DISABLE);
  config.set("observation.sender.implementation", "OBSERVATION_EH_SENDER");
  batcher_config = utility::get_batcher_config(config, OBSERVATION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::DISABLE);
  config.set("observation.sender.implementation", "OBSERVATION_FILE_SENDER");
  batcher_config = utility::get_batcher_config(config, OBSERVATION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::DISABLE);
  config.set("observation.sender.implementation", "OBSERVATION_HTTP_API_SENDER");
  batcher_config = utility::get_batcher_config(config, OBSERVATION_SECTION);
  BOOST_ASSERT(batcher_config.events_counter_status == events_counter_status::DISABLE);
}
