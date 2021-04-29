// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include "crimson/os/seastore/onode_manager/staged-fltree/fltree_onode_manager.h"
#include "crimson/os/seastore/onode_manager/staged-fltree/stages/key_layout.h"

namespace {
[[maybe_unused]] seastar::logger& logger() {
  return crimson::get_logger(ceph_subsys_test);
}
}

namespace crimson::os::seastore::onode {

FLTreeOnodeManager::contains_onode_ret FLTreeOnodeManager::contains_onode(
  Transaction &trans,
  const ghobject_t &hoid)
{
  return tree.contains(
    trans, hoid
  ).handle_error(
    contains_onode_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in FLTreeOnodeManager::contains_onode"
    }
  );
}

FLTreeOnodeManager::get_onode_ret FLTreeOnodeManager::get_onode(
  Transaction &trans,
  const ghobject_t &hoid)
{
  return tree.find(
    trans, hoid
  ).safe_then([this, &hoid](auto cursor)
              -> get_onode_ret {
    if (cursor == tree.end()) {
      logger().debug(
        "FLTreeOnodeManager::{}: no entry for {}",
        __func__,
        hoid);
      return crimson::ct_error::enoent::make();
    }
    auto val = OnodeRef(new FLTreeOnode(cursor.value()));
    return seastar::make_ready_future<OnodeRef>(
      val
    );
  }).handle_error(
    get_onode_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in FLTreeOnodeManager::get_onode"
    }
  );
}

FLTreeOnodeManager::get_or_create_onode_ret
FLTreeOnodeManager::get_or_create_onode(
  Transaction &trans,
  const ghobject_t &hoid)
{
  if (hoid.hobj.oid.name.length() + hoid.hobj.nspace.length()
      > key_view_t::MAX_NS_OID_LENGTH) {
    return crimson::ct_error::value_too_large::make();
  }
  return tree.insert(
    trans, hoid,
    OnodeTree::tree_value_config_t{sizeof(onode_layout_t)}
  ).safe_then([&trans, &hoid](auto p)
              -> get_or_create_onode_ret {
    auto [cursor, created] = std::move(p);
    auto val = OnodeRef(new FLTreeOnode(cursor.value()));
    if (created) {
      logger().debug(
        "FLTreeOnodeManager::{}: created onode for entry for {}",
        __func__,
        hoid);
      val->get_mutable_layout(trans) = onode_layout_t{};
    }
    return seastar::make_ready_future<OnodeRef>(
      val
    );
  }).handle_error(
    get_or_create_onode_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in FLTreeOnodeManager::get_or_create_onode"
    }
  );
}

FLTreeOnodeManager::get_or_create_onodes_ret
FLTreeOnodeManager::get_or_create_onodes(
  Transaction &trans,
  const std::vector<ghobject_t> &hoids)
{
  return seastar::do_with(
    std::vector<OnodeRef>(),
    [this, &hoids, &trans](auto &ret) {
      ret.reserve(hoids.size());
      return crimson::do_for_each(
        hoids,
        [this, &trans, &ret](auto &hoid) {
          return get_or_create_onode(trans, hoid
          ).safe_then([&ret](auto &&onoderef) {
            ret.push_back(std::move(onoderef));
          });
        }).safe_then([&ret] {
          return std::move(ret);
        });
    });
}

FLTreeOnodeManager::write_dirty_ret FLTreeOnodeManager::write_dirty(
  Transaction &trans,
  const std::vector<OnodeRef> &onodes)
{
  return crimson::do_for_each(
    onodes,
    [this, &trans](auto &onode) -> OnodeTree::btree_future<> {
      auto &flonode = static_cast<FLTreeOnode&>(*onode);
      switch (flonode.status) {
      case FLTreeOnode::status_t::MUTATED: {
        flonode.populate_recorder(trans);
        return seastar::now();
      }
      case FLTreeOnode::status_t::DELETED: {
        return tree.erase(trans, flonode);
      }
      case FLTreeOnode::status_t::STABLE: {
        return seastar::now();
      }
      default:
        __builtin_unreachable();
      }
    }).handle_error(
      write_dirty_ertr::pass_further{},
      crimson::ct_error::assert_all{
        "Invalid error in FLTreeOnodeManager::write_dirty"
      }
    );
}

FLTreeOnodeManager::erase_onode_ret FLTreeOnodeManager::erase_onode(
  Transaction &trans,
  OnodeRef &onode)
{
  auto &flonode = static_cast<FLTreeOnode&>(*onode);
  flonode.mark_delete();
  return erase_onode_ertr::now();
}

FLTreeOnodeManager::list_onodes_ret FLTreeOnodeManager::list_onodes(
  Transaction &trans,
  const ghobject_t& start,
  const ghobject_t& end,
  uint64_t limit)
{
  return tree.lower_bound(trans, start
  ).safe_then([this, &trans, end, limit] (auto&& cursor) {
    using crimson::os::seastore::onode::full_key_t;
    return seastar::do_with(
        limit,
        std::move(cursor),
        list_onodes_bare_ret(),
        [this, &trans, end] (auto& to_list, auto& current_cursor, auto& ret) {
      using get_next_ertr = typename OnodeTree::btree_ertr;
      return crimson::do_until(
          [this, &trans, end, &to_list, &current_cursor, &ret] () mutable
          -> get_next_ertr::future<bool> {
        if (current_cursor.is_end() ||
            current_cursor.get_ghobj() >= end) {
          std::get<1>(ret) = end;
          return seastar::make_ready_future<bool>(true);
        }
        if (to_list == 0) {
          std::get<1>(ret) = current_cursor.get_ghobj();
          return seastar::make_ready_future<bool>(true);
        }
        std::get<0>(ret).emplace_back(current_cursor.get_ghobj());
        return tree.get_next(trans, current_cursor
        ).safe_then([&to_list, &current_cursor] (auto&& next_cursor) mutable {
          // we intentionally hold the current_cursor during get_next() to
          // accelerate tree lookup.
          --to_list;
          current_cursor = next_cursor;
          return seastar::make_ready_future<bool>(false);
        });
      }).safe_then([&ret] () mutable {
        return seastar::make_ready_future<list_onodes_bare_ret>(
            std::move(ret));
      });
    });
  }).handle_error(
    list_onodes_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in FLTreeOnodeManager::list_onodes"
    }
  );
}

FLTreeOnodeManager::~FLTreeOnodeManager() {}

}
