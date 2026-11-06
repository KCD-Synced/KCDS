#include "kcd2s/replication/schema.hpp"

#include "kcd2s/testkit.hpp"

using namespace kcd2s::replication;

static SchemaRegistry make_human_schema() {
  SchemaRegistry reg;
  reg.define("Human")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner, {-4096, 4096, 20})
      .field("rotation", FieldKind::Quat, Priority::High, Authority::Owner, {0, 0, 9})
      .field("health", FieldKind::F32, Priority::Med, Authority::Server, {0, 1, 10})
      .field("name", FieldKind::Str, Priority::Low, Authority::Server);
  return reg;
}

KCD2S_TEST(schema, define_and_lookup) {
  SchemaRegistry reg = make_human_schema();
  KCD2S_CHECK_EQ(reg.type_count(), std::size_t{1});
  const TypeDef* td = reg.get("Human");
  KCD2S_CHECK(td != nullptr);
  KCD2S_CHECK_EQ(td->id, kcd2s::core::type_id("Human"));
  KCD2S_CHECK_EQ(td->field_count(), std::size_t{4});
}

KCD2S_TEST(schema, field_ids_sequential) {
  SchemaRegistry reg = make_human_schema();
  const TypeDef* td = reg.get("Human");
  KCD2S_CHECK_EQ(td->fields[0].id, kcd2s::core::FieldId{0});
  KCD2S_CHECK_EQ(td->fields[3].id, kcd2s::core::FieldId{3});
  KCD2S_CHECK(td->find("health") != nullptr);
  KCD2S_CHECK_EQ(td->find("health")->id, kcd2s::core::FieldId{2});
  KCD2S_CHECK(td->find("missing") == nullptr);
}

KCD2S_TEST(schema, position_field_detected) {
  SchemaRegistry reg = make_human_schema();
  const TypeDef* td = reg.get("Human");
  KCD2S_CHECK_EQ(td->position_field, kcd2s::core::FieldId{0});
}

KCD2S_TEST(schema, authority_and_priority_recorded) {
  SchemaRegistry reg = make_human_schema();
  const TypeDef* td = reg.get("Human");
  KCD2S_CHECK(td->find("position")->auth == Authority::Owner);
  KCD2S_CHECK(td->find("health")->auth == Authority::Server);
  KCD2S_CHECK(td->find("position")->prio == Priority::High);
}

KCD2S_TEST(schema, unknown_type_null) {
  SchemaRegistry reg;
  KCD2S_CHECK(reg.get("Nope") == nullptr);
}
