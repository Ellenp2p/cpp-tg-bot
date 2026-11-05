#pragma once

// Warden 命令策略表：所有命令的“谁能用、在哪儿用、菜单怎么显示”集中在这里。
// 业务处理仍注册在 main.cpp；改行为先改这张表，再看对应 handler。
//
// 字段说明：
//   privateAllowed   私聊是否允许
//   groupAllowed     群里是否允许
//   groupAdminOnly   群里是否仅群管理员/owner（privateAllowed 不受影响）
//   ownerOnly        是否仅 WARDEN_OWNERS
//   groupMenu        是否出现在群管理员 / 菜单
//   privateMenu      是否出现在私聊 / 菜单
//   ephemeralCommand 群菜单项是否 is_ephemeral（命令消息仅发送者可见）
//   deleteBareCommand 没写 @ 的本命令是否立即删除；写了 @本bot 的始终删除
//   groupDeny        仅当 groupAllowed=false 时生效：忽略 / 提示去私聊；
//                    groupAllowed=true 时填 kNotApplicable（字段不生效）

#include <array>
#include <string_view>

namespace warden {

enum class GroupDeny {
    kNotApplicable, // groupAllowed=true：字段不生效
    kIgnore,        // groupAllowed=false：群里静默忽略
    kRedirect,      // groupAllowed=false：群里提示去私聊
};

struct CommandPolicy {
    std::string_view name;
    std::string_view description;
    bool privateAllowed = false;
    bool groupAllowed = false;
    bool groupAdminOnly = true;
    bool ownerOnly = false;
    bool groupMenu = false;
    bool privateMenu = false;
    bool ephemeralCommand = true;
    // 群命令没带 @本bot 时是否也立即删除；带 @ 的始终删除
    bool deleteBareCommand = false;
    GroupDeny groupDeny = GroupDeny::kNotApplicable;
};

inline constexpr std::array<CommandPolicy, 11> kCommandPolicies{{
    //  命令        描述                              私聊   群   群管   owner  群菜单 私聊菜单
    //  ephemeral 删裸命令 群拒绝
    {"start",
     "开始使用 / 功能介绍",
     true,
     false,
     false,
     false,
     false,
     true,
     false,
     false,
     GroupDeny::kIgnore},
    {"help",
     "我都能做什么",
     true,
     false,
     false,
     false,
     false,
     true,
     false,
     false,
     GroupDeny::kIgnore},
    // autodel/status 不用 ephemeral：ephemeral command 的 message_id=0，
    // bot 删不掉；普通命令才能“出现后立即 deleteMessage”。
    {"autodel",
     "本群自动清理：查看 / on / off / 类型 时限",
     false,
     true,
     true,
     false,
     true,
     false,
     false,
     true,
     GroupDeny::kNotApplicable},
    {"ignore",
     "回复某人，忽略他的媒体（可指定类型 / admin）",
     false,
     true,
     true,
     false,
     true,
     false,
     false,
     true,
     GroupDeny::kNotApplicable},
    {"restore",
     "回复某人，恢复他的媒体自动删除",
     false,
     true,
     true,
     false,
     true,
     false,
     false,
     true,
     GroupDeny::kNotApplicable},
    {"status",
     "我的管理员权限（仅操作者可见）",
     false,
     true,
     true,
     false,
     true,
     false,
     false,
     true,
     GroupDeny::kNotApplicable},
    {"id", "我的用户 ID", true, false, false, false, false, true, false, false, GroupDeny::kIgnore},
    {"panel",
     "控制台（私聊）",
     true,
     false,
     false,
     false,
     false,
     true,
     false,
     false,
     GroupDeny::kIgnore},
    {"mygroups",
     "我管理的群（私聊）",
     true,
     false,
     false,
     false,
     false,
     true,
     false,
     false,
     GroupDeny::kIgnore},
    {"stats",
     "运行统计（owner 私聊）",
     true,
     false,
     false,
     true,
     false,
     false,
     false,
     false,
     GroupDeny::kIgnore},
    {"alerttest",
     "测试告警通道（owner 私聊）",
     true,
     false,
     false,
     true,
     false,
     false,
     false,
     false,
     GroupDeny::kIgnore},
}};

inline const CommandPolicy* findCommandPolicy(std::string_view name) {
    for (const auto& policy : kCommandPolicies) {
        if (policy.name == name) {
            return &policy;
        }
    }
    return nullptr;
}

} // namespace warden
