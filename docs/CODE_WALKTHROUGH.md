# 关键代码讲解稿：实现是怎么写出来的

> 配套文档：`docs/PROJECT_WALKTHROUGH.md`（讲架构与取舍）→ 本文讲**具体实现**。
> 用法：每节都是 **目标 → 关键代码 → 逐段解释 → 容易被追问的地方**，
> 引用行号以当前代码为准。
>
> 建议只挑 **§2 行为树 / §4 感知 / §5 EQS / §7 开火** 四节讲深，其余按追问展开。

---

## §1 主循环：固定步长 + 输入锁存

### 目标
逻辑固定 60Hz、渲染不限帧；并且**输入事件不能因为帧率不等频而丢失**。

### 关键代码 1：逻辑步的"发放"

`src/engine/Time.cpp:83`

```cpp
bool Time::consumeFixedStep() noexcept {
    if (accumulator_ < frame_.fixedDelta) {
        return false;
    }

    // 超步保护：丢弃掉无法在预算内消化的时间，保持真实感但避免死亡螺旋。
    if (frame_.stepsThisFrame >= kMaxStepsPerFrame) {
        const double maxConsumable = frame_.fixedDelta * static_cast<double>(kMaxStepsPerFrame);
        accumulator_ = std::min(accumulator_, maxConsumable);
        return false;
    }

    accumulator_ -= frame_.fixedDelta;
    ++frame_.stepsThisFrame;
    frame_.gameTime += frame_.fixedDelta;
    return true;
}
```

**逐段解释**

1. `accumulator_` 攒"真实时间 × timeScale"，每攒够一个 `fixedDelta` 就发放一个逻辑步。
   `consumeFixedStep()` 是**唯一的发放口**，主循环 `while (time.consumeFixedStep())` 用 while 而不是 if，
   所以掉帧时会自动补步。
2. **超步保护**是必需的：一次长卡顿（比如切窗口）会攒下几秒，若不设上限就会
   在一个渲染帧里跑几百个逻辑步 → 更卡 → 攒更多，即"死亡螺旋"。
   注意实现用的是 `min(accumulator_, 上限)`，**不是清空** —— 被丢弃的时间不会凭空变成逻辑时间。
3. `gameTime` 只在发放逻辑步时前进，所以**慢动作/暂停**只需改 `timeScale_` 或让 `consumeFixedStep` 返回 false
   （`Time::beginFrame` 在 `paused_` 时不累积；单步请求伪造一个小累积量）。

### 关键代码 2：输入边沿的跨帧锁存

`src/engine/Input.cpp:243`（键盘采样）

```cpp
        const bool nowDown =
            testInputOverride_ ? testKeyDown_[static_cast<size_t>(i)]
                               : IsKeyDown(static_cast<KeyboardKey>(i));

        state.pressed  = nowDown && !state.down;
        state.released  = !nowDown && state.down;
        state.down      = nowDown;

        if (state.pressed) {
            state.pressedLatched = true;
            state.latchFrames    = 0;
        } else if (state.pressedLatched && ++state.latchFrames > kMaxLatchFrames) {
            state.pressedLatched = false;
            state.latchFrames    = 0;
        }
```

`src/engine/Input.cpp:361`（消费）

```cpp
bool Input::consumeKeyPressed(KeyboardKey key) {
    KeyState* state = keyState(key);
    if (!state || !state->pressedLatched) {
        return false;
    }
    // 取走锁存事件：同一个事件只会被返回一次，无论调用方是渲染帧还是逻辑步。
    state->pressedLatched = false;
    state->latchFrames    = 0;
    state->consumed       = true;
    return true;
}
```

**逐段解释**

1. `state.pressed = nowDown && !state.down` 是**自算边沿**（不依赖 raylib 的 `IsKeyPressed`），
   所以它天然只存活一帧。
2. 问题来了：`Input::update()` 每**渲染帧**调用，而 `consumeKeyPressed` 在**固定逻辑步**里调用。
   120FPS 渲染 + 60Hz 逻辑时，近一半的渲染帧**没有逻辑步**，落在那些帧里的"按下"就会整帧蒸发。
   → 所以按下时额外置一个**锁存位**，由消费方取走；`kMaxLatchFrames = 8` 是防呆上限
   （避免"暂停很久后突然补一枪"）。
3. `isKeyPressed` / `wasActionPressed` 这类**非消费**读取仍然只读 `state.pressed`（严格帧内语义），
   所以调试 UI 按帧读取的行为没有被污染。

### 追问点

- *为什么不用 `IsKeyPressed`？* 它的存活期由 raylib 的帧边界决定，等于把"事件能不能被读到"
  交给了渲染帧率。
- *多跑一个逻辑步"欠"子弹吗？* 不会：锁存位只有一份，被消费即清空，不存在排队。

---

## §2 行为树：tick 生命周期与三种组合节点

### 目标
让"跨帧行为"可以被打断、可以恢复，且 `onEnter/onExit` 严格配对。

### 关键代码 1：统一的 tick 包装

`src/ai/BehaviorTree.cpp:24`

```cpp
NodeStatus BehaviorNode::tick(TickContext& context) {
    ++tickCount_;

    if (!entered_) {          // 首次进入（或中断后重新进入）
        entered_ = true;
        onEnter(context);
    }

    const NodeStatus status = onTick(context);

    if (status == NodeStatus::Running) {
        isRunning_ = true;
    } else {
        if (entered_) {       // 关键语义：只要进入过，结束那一步必须收尾
            entered_ = false;
        }
        isRunning_ = false;
        onExit(context, status);
    }

    lastStatus_ = status;
    return status;
}
```

**逐段解释**

1. 派生类**只实现 `onTick`**，进入/退出/状态记账全在基类这一处完成 ——
   这就是"配对"能被保证的原因：没有任何派生类能绕过它。
2. `onExit` 在**非 Running 的当步**就会调用（哪怕 `onEnter → onTick → Success` 在同一步发生），
   所以"释放锁 / 提交事务 / 记录统计"不会漏。
3. `abort()`（`BehaviorTree.cpp:56`）用于被更高优先级分支抢占：它只对"进入过"的节点调 `onExit`，否则是 no-op。

### 关键代码 2：Sequence 与 Selector

`src/ai/BehaviorTree.cpp:93`（Sequence，剪枝式"与"）

```cpp
    while (currentIndex_ < children_.size()) {
        BehaviorNode* child = children_[currentIndex_].get();
        const NodeStatus status = child->tick(context);

        if (status == NodeStatus::Running) {
            return NodeStatus::Running;      // 停在这里，下步从同一个子节点继续
        }
        if (status == NodeStatus::Failure) {
            onReset();                       // 失败要重置进度，否则下次从中间开始
            return NodeStatus::Failure;
        }
        ++currentIndex_;                     // Success：同一步里继续下一个子节点
    }
    onReset();
    return NodeStatus::Success;
```

`src/ai/BehaviorTree.cpp:150`（Selector，反应式部分）

```cpp
    const size_t runningIndex = currentIndex_;
    const bool   hasRunning   = runningIndex < children_.size();

    for (size_t i = 0; i < children_.size(); ++i) {
        BehaviorNode* child = children_[i].get();
        const NodeStatus status = child->tick(context);

        if (status == NodeStatus::Running) {
            if (hasRunning && i != runningIndex) {
                children_[runningIndex]->abort(context);   // 别的分支接管了，旧的才收尾
            }
            currentIndex_ = i;
            return NodeStatus::Running;
        }
        if (status == NodeStatus::Success) { /* 同理 abort 旧分支并重置 */ }
        // Failure：尝试下一个优先级更低的分支
    }
```

**逐段解释**

1. Sequence 的 `onReset()` 很关键：失败后如果不重置 `currentIndex_`，
   下次进入会**从中间开始**，前置条件（例如"目标可见"）就被跳过了。
2. Selector 有两种语义：
   - **非反应式**（默认）：`currentIndex_` 记住当前分支，它不结束就不回头看高优先级分支 ——
     对"撤退 > 交战 > 搜索 > 巡逻"这种根选择器是**致命**的（落到永远 Running 的巡逻分支后，
     再也发现不了敌人）。
   - **反应式**（`setReactive(true)`，用工厂函数 `reactiveSel()` 构造，`BehaviorTree.inl:74`）：
     每步从 index 0 重新评估。
3. 反应式的**正确写法**是"先从 0 重评，只有当**别的**分支真正接管时才 abort 旧分支"。
   旧实现是"先无条件 abort，再从头重评"，于是任何 index>0 的跨帧行为（等待、移动到点、
   持续射击）每帧被 abort→re-enter，状态归零、永不完成。
   → 这个 bug 是我在实测里发现的（修复后有一个旧测试反而失败了，见 §7 追问点）。

### 关键代码 3：Parallel（移动与射击并行）

`src/ai/BehaviorTree.cpp:265`

```cpp
    for (size_t i = 0; i < children_.size(); ++i) {
        // 已结束的子节点不再 tick，避免重复触发副作用
        // （例如「射击」动作被 tick 两次会消耗两发子弹）
        if (childStatuses_[i] == NodeStatus::Success ||
            childStatuses_[i] == NodeStatus::Failure) {
            if (childStatuses_[i] == NodeStatus::Success) ++successCount;
            else ++failureCount;
            continue;
        }
        const NodeStatus status = children_[i]->tick(context);
        childStatuses_[i] = status;
        ...
    }

    const bool failureReached = (failurePolicy_ == FailurePolicy::RequireOne)
                                    ? (failureCount > 0)
                                    : (failureCount == static_cast<int>(children_.size()));
    if (failureReached) return NodeStatus::Failure;
```

**逐段解释**

1. "已结束的子节点不再 tick"是并行节点的**副作用安全阀**：并行节点的子节点会一直存在，
   若不记住它们的状态，`AimAndFire` 会在同一步被 tick 多次 → 一次开火变成几发子弹。
2. 策略用两个枚举表达：本项目交战分支用 `SuccessPolicy::RequireOne + FailurePolicy::RequireAll`，
   语义是"移动或射击**任一**成功即视为本轮成功；两个都失败才算失败"。这就实现了
   "开枪不必等走位到位，走位也不必等枪口对准"。

### 追问点

- *为什么条件节点默认不缓存？* 本树的条件描述的都是**外部世界**（是否看见目标、距上次看见多久），
  缓存会把第一次求值的结果永久冻结（"敌人一旦没看见玩家就再也看不见"）。
  代价是每 tick 重算 —— 树很小，实测无压力。
- *abort 和 reset 的区别？* `abort` 给"正在运行"的节点一次收尾机会（保留它"运行过"的语义），
  `reset` 是彻底清零、下次重新 `onEnter`。

---

## §3 黑板：类型擦除的代价与补偿

`src/ai/Blackboard.h`

```cpp
    template <typename T>
    uint64_t set(std::string_view key, T&& value, std::string_view writer = "unknown",
                 double gameTime = kNoWriteTime) {
        BlackboardEntry& entry = entries_[std::string(key)];
        entry.value      = std::forward<T>(value);
        entry.typeIndex  = std::type_index(typeid(std::decay_t<T>));
        entry.lastWriter = std::string(writer);
        entry.writeCount++;
        entry.version++;

        if (gameTime != kNoWriteTime) {
            entry.lastWriteTime = gameTime;      // 哨兵值 = 本次不更新写入时间
        }
        ...
        return entry.version;
    }

    template <typename T>
    [[nodiscard]] std::optional<T> get(std::string_view key) const {
        const auto it = entries_.find(std::string(key));
        if (it == entries_.end() || !it->second.value.has_value()) return std::nullopt;
        if (it->second.typeIndex != std::type_index(typeid(T))) return std::nullopt;  // ← 静默返回空
        try {
            return std::any_cast<T>(it->second.value);
        } catch (const std::bad_any_cast&) {
            return std::nullopt;                 // 双保险：类型索引与实际 any 不一致也不崩
        }
    }

    template <typename T>
    [[nodiscard]] const T* getPtr(std::string_view key) const;   // 零拷贝读取
```

**逐段解释**

1. 每条记录带 **类型索引**：读的时候类型不符就返回 `nullopt`。
   这是**静默失败**（不抛异常、不打日志），所以项目里定了一条硬规矩：
   **实体 ID 一律用 `EntityId`（`uint32_t` 别名）读写**，绝不用裸 `int` ——
   曾经因此让整条感知链路失效（写 `vector<int>`、读 `vector<EntityId>`）。
2. **版本号**：让读方能判断"这条数据是不是我上次看到的那条"（也用于调试显示）。
   **写者 + 写时间**：调试面板上直接显示"这条数据谁写的、什么时候写的" ——
   正是这个需求暴露出 `lastWriteTime` 恒为 0（`set()` 当时根本没有时间参数）。
3. `get<T>()` 按值返回 → 读 `vector<vec2>`（路径）会整份拷贝；
   所以补了 `getPtr<T>()` 返回 `const T*`，在热路径（寻路结果、威胁源列表）上用零拷贝读。

### 追问点

- *为什么不用 `variant` / 编译期键？* 那是更好的设计（类型安全 + 无字符串）。
  当时的取舍是"先跑起来、键用可读字符串便于调试"，代价就是本条的类型擦除陷阱。
  如果重做，我会用"编译期句柄 + 类型安全容器"。

---

## §4 感知：四道关卡与"记忆 ≠ 看见"

### 目标
限制 AI 的信息来源（不许开挂），并把"看得见"与"记得"分成两个可查询的事实。

### 关键代码 1：视野锥

`src/ai/Perception.cpp:64`

```cpp
    const glm::vec2 toTarget = targetPosition - selfPosition;
    const float distance = length(toTarget);

    if (distance > config.viewDistance) return false;          // 太远
    if (distance <= config.proximityRadius) return true;       // 贴脸：余光/听觉也能察觉
    const float angle = angleTo(selfFacing, toTarget);
    return std::fabs(angle) <= config.halfFovAngle;            // ±45°
```

### 关键代码 2：可见性 = 视野 AND 视线

`src/ai/Perception.cpp:88`

```cpp
bool PerceptionSystem::canSee(glm::vec2 selfPosition, glm::vec2 selfFacing,
                              const PerceptTarget& target) const {
    if (!target.alive) return false;

    if (!isInFieldOfView(selfPosition, selfFacing, target.position, config_)) {
        return false;                                   // 第一关：视野锥
    }
    if (!config_.enableLineOfSight || !losQuery_) {
        return true;                                    // 第二关关闭：直接可见
    }

    ++lineOfSightChecks_;                               // 计数器：调试面板可见
    return losQuery_(selfPosition, target.position);    // 第二关：地形遮挡
}
```

**要点**：`losQuery_` 是一个注入的 `std::function<bool(vec2, vec2)>`，
所以感知层**完全不认识游戏世界**（也不认识导航网格）；测试里塞一个"恒真/假"的 lambda 就能单测。

### 关键代码 3：确认与记忆

`src/ai/Perception.cpp:176`（节选）

```cpp
        record.visibleNow = visible;

        if (visible) {
            record.lastSeenTime       = gameTime;          // 真正的"最后看见时刻"
            record.lastKnownPosition  = target.position;   // 搜索分支要用的记忆点
            record.distance           = length(target.position - selfPosition);

            // 反应延迟：持续可见足够久才算「确认」
            const double visibleFor = gameTime - record.firstSeenTime;
            if (!record.confirmed && visibleFor >= config_.reactionTime) {
                record.confirmed = true;
            }
            ...
        } else {
            // 丢失视线：重置确认计时，但保留 confirmed 状态，
            // 使其在 memoryDuration 内仍可被 AI 利用（即「记忆」）。
            record.firstSeenTime = gameTime;

            const double sinceSeen = gameTime - record.lastSeenTime;
            if (sinceSeen > config_.memoryDuration) {
                record.confirmed = false;
            }
        }
```

**逐段解释（这段是全项目最重要的语义）**

1. `visible` 是"此刻看得见"；`confirmed` 是"我已经确认过他"。
   **丢失视线后 `confirmed` 仍为真，直到 `memoryDuration`（6 秒）过去。**
2. 因此 `hasConfirmedTarget() == true` **不代表看得见** —— 它只说明"我还记得他"。
   这条不变量被写进 `docs/ROADMAP.md` 的"经验"一节，但它同时也是 bug 重灾区：
   - 用它当**交战分支的条件** ⇒ 玩家消失后敌人继续对着记忆位置开枪（§7）；
   - 用它判断**"重新发现目标"** ⇒ 搜索的"已放弃"标记每步被清掉，敌人被钉在原地无限重播观察动作。
3. 正确的"看得见吗"信号是 `timeSinceLastSeen(gameTime)`（只在**真正看见**时归零）：

```cpp
// src/game/Enemy.cpp:1078  只有"此刻真的看见"才算重新接触
constexpr double kSeenNowTolerance = 0.05;   // ≈ 3 个逻辑步
const bool seenNow = perception_->timeSinceLastSeen(gameTime) <= kSeenNowTolerance;
```

### 追问点

- *记忆有什么用？* 两件事：① 让"搜索分支"有方向（走去最后已知位置）；
  ② 让 `threatLevel` 随时间衰减而不是瞬间归零，撤退/警戒决策因此更自然。
- *怎么证明 AI 没开挂？* `PerceptTarget` 只有位置/阵营/存活/威胁权重，没有速度、没有意图；
  敌人的提前量是靠自己先后两次**观察到的位置**差分出来的（`predictedAimPoint`）。

---

## §5 EQS：打分曲线、NaN 防护与排序

### 关键代码 1：归一化 + 曲线 + NaN 防护

`src/ai/EQS.cpp:25`

```cpp
float EQSTest::applyCurve(float raw) const {
    if (!std::isfinite(raw)) {
        raw = 0.0f;                       // 非有限值 → 最差，让它被正常淘汰
    }

    const float span = rangeMax - rangeMin;
    if (!std::isfinite(span) || span <= 1e-6f) {
        return (curve == ScoreCurve::Constant) ? 1.0f : 0.0f;   // 区间非法：避免除零
    }

    float normalized = std::clamp((raw - rangeMin) / span, 0.0f, 1.0f);

    switch (curve) {
        case ScoreCurve::Linear:        return normalized;              // 越大越满意
        case ScoreCurve::InverseLinear: return 1.0f - normalized;       // 越小越满意
        case ScoreCurve::Quadratic:     return (1.0f - normalized) * (1.0f - normalized);
        case ScoreCurve::Constant:      return 1.0f;
    }
    return 0.0f;
}
```

**为什么 NaN 防护是必须的**：`std::clamp` 对 NaN **原样返回**，
于是 `score < minAcceptableScore` 恒为 false（NaN 参与的比较全部为 false）→
这个候选不但不被淘汰，还会带着 NaN 进入 `stable_sort` → 比较器失去严格弱序 = **UB**。
一个"看起来无害"的除法就能让排序崩溃，所以统一在入口把非有限值压成 0。

### 关键代码 2：一次查询的完整流程

`src/ai/EQS.cpp:336`（节选）

```cpp
    std::vector<glm::vec2> rawPoints = query.generator(context);   // ① 生成候选

    for (const glm::vec2& point : rawPoints) {
        EQSCandidate candidate;
        candidate.position = point;

        if (query.candidateFilter && !query.candidateFilter(point, context)) {
            candidate.rejectReason = "未通过查询级过滤";             // ②-a 硬过滤（能站吗）
            result.rejected.push_back(std::move(candidate));
            continue;
        }

        for (const EQSTest& test : query.tests) {
            if (test.filter && !test.filter(point, context)) {       // ②-b 单项硬约束
                candidate.rejectReason = test.rejectReason;
                rejected = true;
                break;                                              // 过滤器优先，不算分
            }
            const float raw = test.measure(point, context);
            candidate.testScores.push_back(test.applyCurve(raw));    // ③ 归一化打分
        }

        // ④ 加权平均（全零权重视为等权）
        float weighted = 0.0f;
        for (size_t i = 0; i < query.tests.size(); ++i) {
            weighted += candidate.testScores[i] * query.tests[i].weight;
        }
        candidate.score = weighted / totalWeight;

        if (candidate.score < query.minAcceptableScore) {            // ⑤ 阈值淘汰
            candidate.rejectReason = "分数低于阈值";
            result.rejected.push_back(std::move(candidate));
            continue;
        }
        result.candidates.push_back(std::move(candidate));
    }
```

`src/ai/EQS.cpp:406`（节选，排序的稳定性）

```cpp
    std::stable_sort(result.candidates.begin(), result.candidates.end(),
                     [](const EQSCandidate& a, const EQSCandidate& b) {
                         if (std::fabs(a.score - b.score) > 1e-6f) return a.score > b.score;
                         if (std::fabs(a.position.x - b.position.x) > 1e-6f)
                             return a.position.x < b.position.x;       // 同分：按位置定序
                         return a.position.y < b.position.y;
                     });
```

**逐段解释**

1. **过滤器优先于打分**：硬约束（"能站吗""在射程内吗"）先判，不满足就不算分 ——
   既省算力，又让调试面板能显示**淘汰原因**（`rejectReason`），这是"AI 为什么不去那儿"的直接证据。
2. **同分时按位置定序**是为了**可复现**：否则 EQS 会在两个等分点之间来回抖动，
   视觉上就是"AI 原地抽搐"。
3. `elapsedMs` / `rejected` 都被保留在结果里 → 调试面板能显示"这次查询花了多久、淘汰了多少候选"。

### 追问点

- *曲线方向搞反过吗？* 搞反过：距离测试在"无目标"时返回 0，归一化后是最低分（注释却写着"给满分"），
  结果没有目标时 AI 反而选不出撤退点。
- *候选点上限*：`gridGenerator` 把半径钳到最多 65×65≈4.2k 个候选，
  防止有人传 `spacing=1, radius=2000` 导致一帧申请 1600 万个点。

---

## §6 A\*：堆、对角与部分路径

`src/ai/Pathfinding.cpp:792`（主循环节选）

```cpp
    // 搜索过程中 O(1) 维护"离目标最近的已扩展节点"，供部分路径回退使用
    float   bestHeuristic = heuristic(startIndex);
    int32_t bestNode      = startIndex;

    while (expanded < maxExpandedNodes) {
        const int32_t current = heapPop();          // 手写二叉堆（配合 heapPositions_ 反查表）
        if (current < 0) break;                     // 开放列表耗尽：不可达
        if (current == goalIndex) { reachedGoal = true; break; }
        if (closed_[current] != 0) continue;
        closed_[current] = 1;

        const float currentHeuristic = heuristic(current);
        if (currentHeuristic < bestHeuristic) { bestHeuristic = currentHeuristic; bestNode = current; }
        ++expanded;

        const int directionCount = allowDiagonal ? 8 : 4;
        for (int d = 0; d < directionCount; ++d) {
            ...
            const bool isDiagonal = (d >= kOrthogonalDirectionCount);
            if (isDiagonal) {
                const GridCoord sideA{cx + kDirX[d], cy};
                const GridCoord sideB{cx, cy + kDirY[d]};
                if (!grid.isWalkable(sideA) && !grid.isWalkable(sideB)) {
                    continue;                       // 两正交邻居都是墙 → 禁止斜穿墙角
                }
            }
            const float stepCost = grid.cellSize() * (isDiagonal ? config_.diagonalCost : 1.0f);
            const float tentativeG = gScore_[current] + stepCost * terrainCost;
            if (tentativeG < gScore_[neighborIndex]) { ... fScore = g + heuristic(neighbor) ... }
        }
    }
```

**逐段解释**

1. **手写二叉堆**（`heapSiftUp/heapSiftDown/heapPop`，配 `heapPositions_` 反查）：
   当某个节点 g 值下降时需要"降低键值并上浮"，标准 `priority_queue` 做不到，
   所以自己维护"节点 ↔ 堆槽位"的双向索引。
2. **穿墙角检查**：对角移动时若两个正交邻居**都是墙**，禁止斜穿（否则单位视觉上"穿过墙角尖"）。
3. **`bestNode`**：搜索失败时用它做部分路径的终点 —— 这就是"走到尽可能近的地方"的实现，
   而且是 O(1) 维护的（旧实现失败后线性扫 `closed_` 数组找最近点）。

### 部分路径的契约（`Pathfinding.cpp:888`）

```cpp
    if (!reachedGoal) {
        if (!allowPartialPath) {
            result.success = false;      // 调用方要求"到不了就失败"，就必须真的失败
            result.partial = false;
            return result;
        }
        // 否则取 bestNode 作为终点，success = true 且 partial = true
    }
```

**追问点**：这里曾有一个 `if (!result.partial && !true)` 的**死代码占位分支**，
导致 `allowPartialPath = false` 也照样返回"成功的部分路径" ——
上层"到达后才进入下一阶段"的状态机被错误驱动。现在三个字段
（`allowPartialPath` / `allowDiagonal` / `maxExpandedNodes`）全部生效，并有测试守着。

---

## §7 敌人决策：四分支根选择器 + 开火四道门

### 关键代码 1：根选择器的结构

`src/game/Enemy.cpp:458`（节选）

```cpp
    auto root = reactiveSel(name_ + ".Root",

        // 分支 1：撤退 —— 血量低于阈值
        seq(name_ + ".RetreatBranch",
            condition(name_ + ".LowHealth", ...),
            action(name_ + ".FindRetreatPosition", ...),    // EQS 选撤退点
            action(name_ + ".MoveToRetreat", ...)),

        // 分支 2：交战 —— 有可交战目标（且未失联过久）
        seq(name_ + ".EngageBranch",
            condition(name_ + ".HasEngageableTarget", ...),
            action(name_ + ".SelectCombatPosition", ...),   // EQS 选位 + 节流 + 紧急重评估
            par(name_ + ".EngageParallel",
                ParallelNode::SuccessPolicy::RequireOne,
                ParallelNode::FailurePolicy::RequireAll,
                action(name_ + ".CombatMove", ...),
                action(name_ + ".AimAndFire", ...))),       // ← §7.2

        // 分支 3：搜索 —— 有线索（自己的记忆或队友情报）但看不见
        seq(name_ + ".SearchBranch", ...),

        // 分支 4：巡逻 / 待命
        action(name_ + ".Patrol", ...));
```

**讲解要点（优先级即战术意图）**：`撤退 > 交战 > 搜索 > 巡逻`。
反应式选择器保证"每一步都从最高优先级重新看一遍"，所以"正跑着巡逻突然被发现"能立刻切换。
每个分支内部都遵循同一条纪律：**条件节点只描述世界，动作节点才改状态**。

### 关键代码 2：开火四道门（`AimAndFire`）

`src/game/Enemy.cpp`（节选，按执行顺序）

```cpp
    glm::vec2 targetPosition;
    if (!resolveTargetPosition(blackboard_, targetPosition)) return NodeStatus::Failure;
    // ↑ 读的是 kLastKnownPosition = 敌人"记忆"中的位置

    const glm::vec2 aimTarget = predictedAimPoint(targetPosition, context.gameTime);
    aimPoint_ = aimTarget;
    steerFacingToward(normalizeOr(aimTarget - position(), facing_), context.fixedDelta);
    aimSteeredThisStep_ = true;                     // 告诉 applyMovement：这一步朝向归瞄准

    const float distance = vectorLength(targetPosition - position());
    if (distance > config_.fireRange) return NodeStatus::Running;   // 门 0：够不着

    if (blackboard_.getOr<float>(Blackboard::Keys::kTimeSinceSeenEnemy, 1e6f) >
        config_.fireTargetMaxAge) {                // 门 1：目标"新鲜度"
        return NodeStatus::Running;                //    —— 记忆里的位置不许开枪
    }

    if (lateralAimError(aimTarget) > config_.aimHitRadius) {
        return NodeStatus::Running;                // 门 2：弹道横向偏差 > 命中窗口
    }
    if (losQuery_ && !losQuery_(position(), aimTarget)) {
        return NodeStatus::Running;                // 门 3：弹道被掩体挡住
    }
    if (context.gameTime - lastFireTime_ < config_.fireCooldown) {
        return NodeStatus::Running;                // 门 4：冷却
    }

    lastFireTime_ = context.gameTime;
    blackboard_.set(Blackboard::Keys::kShouldFire, true, kDecisionWriter);
    firedThisStep_ = true;                         // 只置"意图"，子弹由 World 生成
    ++shotsFired_;
```

**逐段解释（这段是"AI 手感"最集中的地方）**

1. **先转向再判射程**：这样追击途中枪口始终指向目标，进入射程即可开火，
   而不是"到位之后再花 0.3~0.9 秒把枪口转回来"（实测这部分占了开火间隔的 ~30%）。
2. **门 2 用横向偏差而不是角度**：角度容差的误差随距离放大（11.5° 在 220 单位外偏 44 单位），
   而命中窗口是固定的（目标半径 11 + 子弹半径 3 = 14）。只有横向偏差能与命中窗口直接比较。
3. **所有失败都返回 `Running` 而不是 `Failure`**：`Failure` 会让外层 Sequence 短路，
   整条交战分支（包括移动）失效 → AI 会在冷却期间僵在原地。
4. **只写"意图"不生成子弹**：`firedThisStep_` / 黑板 `kShouldFire` 由 `World::processWeaponFire`
   在阶段 3 统一消费。好处：AI 层不持有子弹生成逻辑，并且"开火次数 == 子弹数"是可断言的不变式。

### 关键代码 3：提前量（不许读目标速度）

`src/game/Enemy.cpp`（`predictedAimPoint` 节选）

```cpp
    const double dt = gameTime - prevAimTime_;
    if (hasPrevAimPoint_ && dt > 1e-4) {
        const glm::vec2 delta = targetPosition - prevAimPoint_;
        const float jump = vectorLength(delta);

        constexpr float kTargetSwitchJump = 120.0f;
        if (jump > kTargetSwitchJump) {
            targetVelocity_ = {0.0f, 0.0f};                // 一步跳 120 单位 = 换目标了
        } else {
            const glm::vec2 raw = delta / static_cast<float>(dt);
            constexpr float kSmoothing = 0.25f;            // 指数平滑：抑制差分噪声
            targetVelocity_ = targetVelocity_ * (1.0f - kSmoothing) + raw * kSmoothing;

            constexpr float kMaxTargetSpeed = 420.0f;      // 安全阀：异常差分不会让提前量失控
            const float speed = vectorLength(targetVelocity_);
            if (speed > kMaxTargetSpeed) {
                targetVelocity_ = targetVelocity_ * (kMaxTargetSpeed / speed);
            }
        }
    }
    prevAimPoint_ = targetPosition;
    prevAimTime_  = gameTime;
    hasPrevAimPoint_ = true;

    // 提前量 = 目标速度 × 子弹飞行时间
    const float distance = vectorLength(targetPosition - position());
    const float flightTime = distance / projectileSpeed_;
    return targetPosition + targetVelocity_ * (flightTime * config_.aimPrediction);
```

**要点**：速度是**自己差分**出来的（两次观察位置之差 / 时间），不是读玩家的速度向量 ——
这就是"不开挂"的具体实现；`aimPrediction = 0.85` 刻意留一点误差，
让"直线跑"会被打中、"急变换向/翻滚"能躲开。

### 追问点

- *修好反应式选择器之后为什么有个旧测试失败了？*
  那条测试是"冷却更短应当打出更多子弹"。修好 AI 之后敌人真的会交战了，
  于是 4 个敌人在 25 秒内把玩家打死，两组都被"打死玩家所需的枪数"截断。
  真正的问题是**测试前提**（玩家会死），修法是把玩家血量设为 1e6。
  这个例子说明：**测试通过不等于测试有效**。

---

## §8 移动：前瞻点、到达判定与朝向

`src/game/Enemy.cpp`（`followPath` 节选）

```cpp
    if (currentPath_.waypoints.empty() || !currentPath_.success) {
        return moveDirectlyToward(destination, fixedDelta);      // 没路径：退化为直线
    }

    const float lookAhead = 24.0f;
    const auto aheadPoint = AI::Pathfinder::pointAhead(currentPath_.waypoints, pathCursor_,
                                                       position(), lookAhead);

    // 路走完了但人还没到：这条是 A* 的**截断路径**（部分路径），不是"到达"
    if (!aheadPoint && destinationDistance >= kArrivalRadius) {
        constexpr double kRepathInterval = 0.5;
        if (currentTime_ - lastPathTime_ >= kRepathInterval) {
            repathRequested_ = true;                              // 节流重算，别每步都跑 A*
        }
        return moveDirectlyToward(destination, fixedDelta);       // 先朝目的地直线靠拢
    }

    if (!aheadPoint || destinationDistance < kArrivalRadius) {
        // 到达：减速停下
        const float damping = std::max(0.0f, 1.0f - 8.0f * delta);
        velocity_ *= damping;
        return destinationDistance < kArrivalRadius;
    }
```

**逐段解释**

1. **前瞻点（look-ahead）**：不对着"下一个路点"走，而是沿折线取前方 24 单位处的一点 ——
   消除"走一步停一步"的机械感。
2. **"路走完"≠"到达"**：这是踩过的坑。部分路径（截断路径）走到头时 `pointAhead` 返回
   `nullopt`，看起来和"到达"一模一样。早期直接当成到达 → AI **永久冻结在半路**
   （实测 4 个敌人有 3 个卡住）。正确做法：视为"路线失效"，节流重算 + 先朝目的地直线靠近。
3. **到达阻尼**：`velocity_ *= (1 - 8·dt)` 让停下是渐进的，不是瞬停。

`steerFacingToward` 用的是**角度插值**而不是向量插值：

```cpp
    const float currentAngle = std::atan2(facing_.y, facing_.x);
    const float desiredAngle = std::atan2(desired.y, desired.x);
    const float error = wrapAngle(desiredAngle - currentAngle);
    constexpr float kTurnRate = 6.0f;                    // ≈344°/s
    const float maxTurn = kTurnRate * static_cast<float>(fixedDelta);
    // 按 maxTurn 朝 error 方向旋转 facing_
```

**为什么不用 `lerp(facing, desired, t)`**：目标在正后方时两向量近似相反，
插值会经过零向量 → 朝向瞬间跳变。角度插值天然处理 180° 情况。

### `applyMovement` 里一条容易忽略的规则

```cpp
    if (!aimSteeredThisStep_) {         // 本步瞄过就不按"移动方向"转
        steerFacingToward(desiredFacing_, fixedDelta);
    }
    const float maxSpeed = desiredMoveSpeed();   // 与 followPath 用同一个速度判据
    clampVelocity(maxSpeed);
```

**要点**：两条都踩过坑 —— ① 朝向若被移动方向"抢走"，会导致"侧移时枪口被甩开、永远打偏"；
② 速度上限若自己写一套判据（只认撤退），`desiredMoveSpeed()` 里新增的
"够不着就冲刺"就会被夹回走速（实测敌人最高速永远是 145，追不上玩家）。

---

## §9 弹道：子步进与三道判定

`src/game/World.cpp:1152`（节选）

```cpp
        const int substeps = std::max(
            1, static_cast<int>(std::ceil(stepDistance / kProjectileMaxSubstep)));  // ≤6 单位
        const float segment = stepDistance / static_cast<float>(substeps);

        for (int step = 0; step < substeps; ++step) {
            const glm::vec2 next = projectile->position() + increment;

            // --- 1. 地形（墙 / 掩体 / 出界）---
            if (projectileBlockedAt(next)) {
                projectile->setPosition(next);
                projectile->markHitTerrain();
                break;
            }
            projectile->setPosition(next);

            // --- 2. 目标 ---
            if (Entity* victim = findProjectileVictim(*projectile)) {
                const bool wasInvulnerable = victim->isInvulnerable();
                const float dealt = applyDamage(victim->id(), projectile->damage(),
                                                projectile->ownerId(), next, gameTime);
                projectile->markHitEntity(victim->id(), dealt);
                if (dealt > 0.0f) { /* 记伤害、记命中 */ }
                else if (wasInvulnerable) { ++totalProjectilesDodged_; }   // 翻滚躲开了
                break;
            }

            // --- 3. 射程 ---
            if (projectile->advance(segment)) { projectile->markExpired(); break; }
        }
```

**逐段解释**

1. **子步进**：60Hz 下子弹每步位移约 13.7 单位，而敌人半径只有 11 ——
   一步跨过会"骑"在目标两侧导致漏检。切成 ≤6 单位后单段位移远小于最小目标直径。
2. **判定顺序**：地形 → 目标 → 射程。地形必须先判，否则"掩体挡子弹"会变成"掩体后面的目标照样中弹"。
3. **无敌帧要被计数**：`totalProjectilesDodged_` 让"翻滚躲开子弹"变成 HUD 上会跳的数字 ——
   无敌帧是"看不见的规则"，没有可见证据就只能读代码相信它生效了。

---

## §10 几何一致性：为什么视线/弹道不用导航网格

`src/game/World.cpp`（`segmentIntersectsRect` 节选）

```cpp
bool segmentIntersectsRect(glm::vec2 from, glm::vec2 to,
                           glm::vec2 min, glm::vec2 max) noexcept {
    const glm::vec2 delta = to - from;
    float tEnter = 0.0f, tExit = 1.0f;

    for (int axis = 0; axis < 2; ++axis) {
        const float origin = (axis == 0) ? from.x : from.y;
        const float dir    = (axis == 0) ? delta.x : delta.y;
        const float lo     = (axis == 0) ? min.x : min.y;
        const float hi     = (axis == 0) ? max.x : max.y;

        if (std::fabs(dir) < 1e-6f) {
            if (origin < lo || origin > hi) return false;    // 平行且在外侧
            continue;
        }
        float t0 = (lo - origin) / dir;
        float t1 = (hi - origin) / dir;
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit  = std::min(tExit, t1);
        if (tEnter > tExit) return false;
    }
    return true;   // 存在 t ∈ [0,1] 落进矩形
}
```

`src/game/World.cpp`（`makeLineOfSightQuery`）

```cpp
    return [this](glm::vec2 from, glm::vec2 to) -> bool {
        for (const auto& entity : entities_) {
            if (entity->type() != EntityType::Obstacle) continue;
            const auto* obstacle = static_cast<const Obstacle*>(entity.get());
            const glm::vec2 half   = obstacle->size() * 0.5f;
            const glm::vec2 center = obstacle->position();
            if (segmentIntersectsRect(from, to, center - half, center + half)) return false;
        }
        return true;
    };
```

**为什么不用导航网格的阻挡格**：`NavGrid::fillRect` 用矩形角点 `floor` 填格，
障碍**压住某格的一部分**就把整格标成阻挡 → 每个掩体的阻挡范围向外多出 0~32 单位。
拿它判视线/弹道会出现"贴墙隐身、子弹离墙一格就消失"（实测：墙面 x=700、
玩家贴到 x=688 时完全看不见，朝他开枪掉血 0）。

**分层原则（直接背下来）**：

| 用途 | 用什么 | 为什么 |
|---|---|---|
| 寻路 / 能否站立 | **导航网格（膨胀格）** `canStand` | 实体有身体半径，必须预留间隙 |
| 视线 / 弹道 / 掩体判定 | **精确几何** | 要的是"看起来挡没挡住"，不是"格子里有没有东西" |

顺带的好处：判定读**活着的实体**而不是派生网格数据，
所以"障碍被移除后留下看不见的墙"这个 bug 对视线/弹道免疫。

---

## 讲法建议

**如果只有 10 分钟**：§2（tick 配对 + 反应式选择器的正确写法）→ §4（confirmed ≠ visible）
→ §7（开火四道门）→ §10（膨胀格 vs 精确几何）。这四段覆盖了
"AI 框架语义 / 信息约束 / 行为调优 / 几何正确性"四个维度，而且每一段都带真实的坑。

**如果只有 3 分钟**：只讲 §4 的不变量 + §7 的门 1。
它俩是因果关系：因为"记得 ≠ 看得见"，所以开火必须额外判"目标新鲜度"。

**最容易被追问的三个点，先准备好答案**：

1. `Running` 和 `Failure` 混用会不会让树失控？
   → 约定：**能继续做的事返回 Running，明确做不到的事返回 Failure**；
   交战分支里的"暂时打不了"（超射程/没对准/冷却/被挡）全是 Running，
   保证 Sequence 不短路、分支不退出。
2. 黑板字符串键 + `std::any` 的性能？
   → 热路径已做补偿（写前比对、`getPtr` 零拷贝、`bindings()` 返回 const 引用），
   实测 AI 每逻辑步（3 个敌人）0.003~0.004ms。设计改进方向见 §3 追问点。
3. 这些行为怎么验证？
   → 三条路：单元测试（279 个用例）、可视化（`1/3/4/5/7` 键 + F1 面板改参数）、
   无头回归（`--headless --frames 3600 --dump`）。修 bug 时必须做**对照实验**：
   回退修复 → 测试必须变红，否则那条测试就是"没牙齿"的。
