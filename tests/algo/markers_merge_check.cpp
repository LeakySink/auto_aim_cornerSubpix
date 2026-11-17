// 复现并验证 markers 合并的崩溃：
//   kalman_markers() / MarkerArray::to_json() 返回的是**对象**
//   {"schema":"marker_v1","frame_id":"world","items":[...]}，不是数组。
//   对对象直接 push_back → 运行期抛 type_error.308（编译期查不出来）。
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

static nlohmann::json make_marker_array()
{
  // 与 tools/rdbg/markers/viz_markers.hpp 的 to_json() 同构
  return {
    {"schema", "marker_v1"},
    {"frame_id", "world"},
    {"items", nlohmann::json::array({{{"ns", "kalman.center"}, {"id", "c"}, {"type", "sphere"}}})},
  };
}

int main()
{
  int fail = 0;

  // ---- 1) 错误写法：直接遍历对象（会抛 type_error.308） ----
  bool threw = false;
  try {
    auto markers = make_marker_array();
    auto extra = make_marker_array();
    for (auto & m : extra) {  // 对象迭代得到的是 pair，push_back 到对象 → 抛异常
      markers.push_back(m);
    }
  } catch (const nlohmann::json::exception & e) {
    threw = true;
    std::printf("  [预期] 旧写法抛异常 id=%d: %s\n", e.id, e.what());
  }
  std::printf("== 1) 旧写法（对对象 push_back）\n  抛出异常=%d\n", threw ? 1 : 0);
  if (!threw) {
    std::printf("  [FAIL] 期望抛异常却没有\n");
    ++fail;
  }

  // ---- 2) 正确写法：追加到 items ----
  std::printf("\n== 2) 新写法（追加到 items）\n");
  try {
    auto markers = make_marker_array();
    const auto extra = make_marker_array();
    if (markers.is_object() && markers.contains("items") && markers["items"].is_array()) {
      for (const auto & item : extra["items"]) markers["items"].push_back(item);
    }
    const auto n = markers["items"].size();
    std::printf("  items 合并后 = %zu（期望 2）\n", static_cast<size_t>(n));
    std::printf("  schema=%s frame_id=%s\n", markers["schema"].get<std::string>().c_str(),
                markers["frame_id"].get<std::string>().c_str());
    if (n != 2) {
      std::printf("  [FAIL] items 数量不对\n");
      ++fail;
    }
    if (markers["schema"] != "marker_v1") {
      std::printf("  [FAIL] schema 被破坏\n");
      ++fail;
    }
  } catch (const std::exception & e) {
    std::printf("  [FAIL] 新写法也抛了: %s\n", e.what());
    ++fail;
  }

  std::printf("\n%s（失败项 %d）\n", fail == 0 ? "全部通过" : "存在失败", fail);
  return fail == 0 ? 0 : 1;
}
