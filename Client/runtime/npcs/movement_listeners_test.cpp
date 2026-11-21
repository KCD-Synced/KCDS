// A freed movement task is taken out of the navigation module's listeners; other lists are left.
#include "movement_listeners.hpp"

#include <map>

#include "kcd2s/testkit.hpp"

using kcds::runtime::npcs::keep_live_tasks;

namespace {

int task_table = 0;
int other_table = 0;

struct Tables {
  std::map<void*, const void*> of;
  const void* operator()(void* listener) const {
    const auto found = of.find(listener);
    return found == of.end() ? nullptr : found->second;
  }
};

}  // namespace

KCD2S_TEST(movement_listeners, freed_tasks_are_taken_out_in_order) {
  int a = 0;
  int b = 0;
  int c = 0;
  int d = 0;
  Tables tables;
  tables.of[&a] = &task_table;
  // Its memory taken by another object: another vtable.
  tables.of[&b] = &other_table;
  tables.of[&c] = &task_table;
  // Its memory gone: nothing readable.
  void* list[] = {&a, &b, &c, &d};
  const std::size_t kept = keep_live_tasks(list, 4, &task_table, tables);
  KCD2S_CHECK_EQ(kept, std::size_t(2));
  KCD2S_CHECK(list[0] == &a);
  KCD2S_CHECK(list[1] == &c);
}

KCD2S_TEST(movement_listeners, a_list_without_a_task_is_left_as_it_is) {
  int a = 0;
  int b = 0;
  Tables tables;
  tables.of[&a] = &other_table;
  tables.of[&b] = &other_table;
  void* list[] = {&a, &b};
  KCD2S_CHECK_EQ(keep_live_tasks(list, 2, &task_table, tables), std::size_t(2));
  KCD2S_CHECK(list[0] == &a);
  KCD2S_CHECK(list[1] == &b);
  KCD2S_CHECK_EQ(keep_live_tasks(list, 0, &task_table, tables), std::size_t(0));
}

KCD2S_TEST(movement_listeners, live_tasks_stay) {
  int a = 0;
  int b = 0;
  Tables tables;
  tables.of[&a] = &task_table;
  tables.of[&b] = &task_table;
  void* list[] = {&a, &b};
  KCD2S_CHECK_EQ(keep_live_tasks(list, 2, &task_table, tables), std::size_t(2));
}
