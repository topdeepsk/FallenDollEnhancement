#pragma once

#include <windows.h>

namespace ge {

// 动作放置（H 卡预览）的"红区也能落位"功能。
//
// 机制：游戏在预览时把判定结果写在 A_ClientHPreviewManager 的
// isSnappingItem / isSnappingWall / FloorPreviewZ / Z / wallHitLoaction 上，
// 左键处理函数读这几个值决定提交还是弹"请放置在平面上"。
// 这里在低级鼠标钩子里、**在输入投递给游戏之前**把状态改写成"已吸附"，
// 于是游戏走自己的提交路径，位置/朝向/网络 RPC 都是原生的。
//
// 钩子回调只做常数次内存读写（偏移在后台线程预先解析好），不会拖慢输入。
bool StartPreviewBypass(HMODULE hSelf);
void StopPreviewBypass();
bool IsPreviewBypassInstalled();

// 立刻按当前预览位置改写一次（等价于按一下左键，但不需要真的点）。
// 返回：0 未就绪 / 1 有管理器但不在预览 / 2 已有效（跳过）/ 3 已改写 / -1 失败
int PreviewApplyNow();

// 0 = 找不到管理器；1 = 管理器在但没进预览；2 = 预览中
int PreviewQueryState();

}  // namespace ge
