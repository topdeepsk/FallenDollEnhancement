#pragma once

namespace ge {

// 跨进程跳跃：这个游戏的玩家位置由服务端权威决定（本地进程里 PC/Pawn 的 Role 是
// AutonomousProxy），客户端写坐标会在下一个网络更新（~32ms）被覆盖回去。
// 所以跳跃必须写到同机运行的另一个同名进程（服务端）里去，由它复制给客户端。
//
// 返回 true 表示已经在服务端写好；false 表示找不到对端 / 对端里没有匹配的角色，
// 调用方可以退回本地写入（会被覆盖，仅作兜底）。
bool PeerJumpUp(const double localWorld[3], double height);

// 关闭缓存（卸载时调用），释放持有的进程句柄。
void PeerJumpShutdown();

}  // namespace ge
