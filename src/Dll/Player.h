#pragma once

namespace ge {

// 跳跃工作线程：等待"请求"事件，收到后解析玩家并抬升 Z。
// 之所以不直接在键盘回调里做，是因为抬升期间要重写若干次（holdMs），
// 在低级键盘钩子的回调里阻塞会拖慢整个系统的输入。
bool StartJumpWorker();
void StopJumpWorker();

// 请求一次跳跃（自动重置事件，连按只排一次）。
void RequestJump();

}  // namespace ge
