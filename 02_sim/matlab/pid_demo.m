%% =====================================================================
%  PID 教学仿真（纯 MATLAB，不需要 Simulink、不需要任何 Toolbox）
%
%  演示 A：速度环 —— P 留下稳态误差，I 才能消掉
%  演示 B：位置环 —— D 就是"刹车"
%  演示 C：积分饱和 —— 输出顶到上限后积分还在涨，会"积重难返"
%
%  用法：保存为 pid_demo.m，在 MATLAB 里按 F5 运行
%  兼容：R2018b 及以上（用了 yline）
%  =====================================================================
clear; clc; close all;

Ts = 0.001;          % 仿真步长 1ms，和实际控制周期一致

%% ========== 演示 A：速度环（等价于底盘 M3508 电机）==========
targetA = 100;
TendA   = 3.0;

casesA = {
    '只有 P（kp=2）',                2,   0,   0;
    'PI，I 太小（kp=2, ki=10）',      2,   10,  0;
    'PI，合适（kp=2, ki=100）',       2,   100, 0;
    'PI，I 太大（kp=2, ki=800）',     2,   800, 0;
};

figure('Name','演示A+B：P 与 D 的作用','Color','w','Position',[80 80 1150 780]);

subplot(2,1,1); hold on; grid on; box on;
fprintf('\n===== 演示 A：速度环（目标 = %g）=====\n', targetA);
fprintf('%-26s %10s %10s %14s\n','参数','稳态值','超调(%)','1%%调节时间(s)');
for c = 1:size(casesA,1)
    y = sim_speed(casesA{c,2}, casesA{c,3}, casesA{c,4}, targetA, Ts, TendA);
    t = (1:numel(y))*Ts;
    plot(t, y, 'LineWidth', 1.5, 'DisplayName', casesA{c,1});
    [ss, ovs, ts] = report(y, targetA, Ts);
    fprintf('%-26s %10.2f %10.1f %14.2f\n', casesA{c,1}, ss, ovs, ts);
end
yline(targetA,'k--','LineWidth',1.2,'DisplayName','目标转速');
xlabel('时间 (s)'); ylabel('转速');
title('演示 A：速度环 —— P 留下稳态误差，I 才能消掉它（但 I 太大会过冲）');
legend('Location','southeast'); xlim([0 TendA]);

%% ========== 演示 B：位置环（等价于云台 / 平衡）==========
targetB = 1.0;
TendB   = 5.0;

casesB = {
    '只有 P（kp=100, kd=0）',     100, 0, 0;
    'PD，D 太小（kp=100, kd=3）', 100, 0, 3;
    'PD，合适（kp=100, kd=20）',  100, 0, 20;
};

subplot(2,1,2); hold on; grid on; box on;
fprintf('\n===== 演示 B：位置环（目标 = %g）=====\n', targetB);
fprintf('%-26s %10s %10s %14s\n','参数','稳态值','超调(%)','1%%调节时间(s)');
for c = 1:size(casesB,1)
    y = sim_pos(casesB{c,2}, casesB{c,3}, casesB{c,4}, targetB, Ts, TendB);
    t = (1:numel(y))*Ts;
    plot(t, y, 'LineWidth', 1.5, 'DisplayName', casesB{c,1});
    [ss, ovs, ts] = report(y, targetB, Ts);
    fprintf('%-26s %10.3f %10.1f %14.2f\n', casesB{c,1}, ss, ovs, ts);
end
yline(targetB,'k--','LineWidth',1.2,'DisplayName','目标位置');
xlabel('时间 (s)'); ylabel('位置');
title('演示 B：位置环 —— 没有 D 会一直振荡，D 就是"刹车"');
legend('Location','southeast'); xlim([0 TendB]);

%% ========== 演示 C：积分饱和（对应讲义"I-更进一步"那页）==========
% 讲义参数：Kp=3, Ki=4, Kd=1.8；这里 Ki 取 1.0 让曲线更易读，现象相同
% 输出（力矩/加速度）被限幅在 ±1.0 —— 这就是"电机的物理上限"
targetC = 1.0;
uMaxC   = 1.0;
TendC   = 80.0;

casesC = {
    '① 不限积分（讲义左图）',              3, 1.0, 1.8, inf;
    '② 积分限幅 I_max=0.15（讲义右图）',   3, 1.0, 1.8, 0.15;
    '③ 积分限幅 I_max=0.40（限得太松）',   3, 1.0, 1.8, 0.40;
};

figure('Name','演示C：积分饱和','Color','w','Position',[120 120 1150 620]);
hold on; grid on; box on;
fprintf('\n===== 演示 C：积分饱和（目标 = %g，输出限幅 = ±%g）=====\n', targetC, uMaxC);
fprintf('%-36s %10s %10s %14s\n','方案','峰值','超调(%)','1%%调节时间(s)');
for c = 1:size(casesC,1)
    y = sim_windup(casesC{c,2}, casesC{c,3}, casesC{c,4}, casesC{c,5}, ...
                   targetC, uMaxC, Ts, TendC);
    t = (1:numel(y))*Ts;
    plot(t, y, 'LineWidth', 1.6, 'DisplayName', casesC{c,1});
    [ss, ovs, ts] = report(y, targetC, Ts);
    fprintf('%-36s %10.2f %10.1f %14.2f\n', casesC{c,1}, max(y), ovs, ts);
end
yline(targetC,'k--','LineWidth',1.2,'DisplayName','目标位置');
xlabel('时间 (s)'); ylabel('位置');
title('演示 C：积分饱和 —— 输出顶到上限后积分还在疯涨，到目标时"刹不住"');
legend('Location','northeast'); xlim([0 TendC]);

fprintf('\n结论：\n');
fprintf('  · 不限积分：峰值冲到约 6（目标才 1），而且 8 秒都回不来 —— 这就是"积重难返"\n');
fprintf('  · 积分限幅 0.15：峰值 1.15，4.2 秒就稳定 —— 平滑到位\n');
fprintf('  · 对应你代码里的 componentKiMax（Motor.cpp 第 23 行 INRANGE）\n');
fprintf('\n提示：改 casesA / casesB / casesC 里的数字，按 F5 重跑，观察曲线怎么变。\n\n');

%% ========== 演示 D：积分限幅的"代价"——有扰动时才暴露 ==========
% 演示 C 是理想模型（静止不需要输出），所以限幅给小了也看不出来。
% 这里给被控对象加一个恒定扰动 d=0.3（模拟摩擦/重力/负载），
% 积分项必须"顶住"这个扰动 —— 限幅太小的代价就出现了：稳态误差。
targetD = 1.0;
uMaxD   = 1.0;
distD   = 0.3;      % ← 改成 0 就退回理想模型，三个曲线会重合
TendD   = 10.0;

casesD = {
    'I_max=0.05（限太紧）',   3, 1.0, 1.8, 0.05;
    'I_max=0.15（讲义值）',   3, 1.0, 1.8, 0.15;
    'I_max=0.40（够用）',     3, 1.0, 1.8, 0.40;
};

figure('Name','演示D：限幅的代价','Color','w','Position',[160 160 1150 620]);
hold on; grid on; box on;
fprintf('\n===== 演示 D：有扰动 d=%g 时，积分限幅的代价 =====\n', distD);
fprintf('%-24s %10s %12s %14s\n','方案','稳态值','稳态误差(%)','1%%调节时间(s)');
for c = 1:size(casesD,1)
    y = sim_dist(casesD{c,2}, casesD{c,3}, casesD{c,4}, casesD{c,5}, ...
                 targetD, uMaxD, distD, Ts, TendD);
    t = (1:numel(y))*Ts;
    plot(t, y, 'LineWidth', 1.8, 'DisplayName', casesD{c,1});
    ss  = mean(y(max(1,end-round(0.5/Ts)):end));
    band = abs(y - targetD) > 0.01*targetD;
    if any(band), ts = find(band,1,'last')*Ts; else, ts = 0; end
    fprintf('%-24s %10.4f %12.1f %14.2f\n', casesD{c,1}, ss, abs(targetD-ss)/targetD*100, ts);
end
yline(targetD,'k--','LineWidth',1.3,'DisplayName','目标位置');
xlabel('时间 (s)'); ylabel('位置');
title(sprintf('演示 D：有恒定扰动时，积分限幅太小会留下稳态误差（d=%g）', distD));
legend('Location','southeast'); xlim([0 TendD]);

fprintf('\n结论：限幅太小 -> 积分顶不住扰动 -> 永远差一点点到不了目标。\n');
fprintf('选值规则：componentKiMax >= 最大负载下维持稳态所需的输出。\n');

%% ==================== 局部函数 ====================
function y = sim_speed(kp, ki, kd, target, Ts, Tend)
% 一阶惯性 y' = (u - y)/tau，tau = 50ms（近似电机速度环）
    N = round(Tend/Ts);
    y = 0; integ = 0; prevErr = 0;
    out = zeros(1,N);
    for k = 1:N
        err     = target - y;
        integ   = integ + err*Ts;          % 注意：这里带 dt（和框架代码不同）
        der     = (err - prevErr)/Ts;
        prevErr = err;
        u = kp*err + ki*integ + kd*der;
        u = max(-5000, min(5000, u));
        y = y + Ts*(u - y)/0.05;
        out(k) = y;
    end
    y = out;
end

function y = sim_pos(kp, ki, kd, target, Ts, Tend)
% 二阶 theta'' = u（近似云台/平衡车角度环）
    N = round(Tend/Ts);
    y = 0; dy = 0; integ = 0; prevErr = 0;
    out = zeros(1,N);
    for k = 1:N
        err     = target - y;
        integ   = integ + err*Ts;
        der     = (err - prevErr)/Ts;
        prevErr = err;
        u = kp*err + ki*integ + kd*der;
        u = max(-5000, min(5000, u));
        dy = dy + Ts*u;
        y  = y  + Ts*dy;
        out(k) = y;
    end
    y = out;
end

function y = sim_windup(kp, ki, kd, imax, target, uMax, Ts, Tend)
% 和框架代码一致：errSum 每次 += err（不带 dt），然后限制"积分项输出"不超过 imax
    N = round(Tend/Ts);
    y = 0; dy = 0; errsum = 0; prevErr = 0;
    out = zeros(1,N);
    for k = 1:N
        err = target - y;
        errsum = errsum + err;                    % 积分累加
        if isfinite(imax)                         % 积分限幅
            errsum = max(-imax/ki, min(imax/ki, errsum));
        end
        der = (err - prevErr)/Ts;
        prevErr = err;
        u = kp*err + ki*errsum + kd*der;
        u = max(-uMax, min(uMax, u));             % ★ 输出限幅（电机物理上限）
        dy = dy + Ts*u;
        y  = y  + Ts*dy;
        out(k) = y;
    end
    y = out;
end

function [ss, ovs, ts] = report(y, target, Ts)
% 稳态值 / 超调百分比 / 1%% 调节时间
    ss  = mean(y(max(1,end-round(0.2/Ts)):end));
    ovs = max(0, (max(y) - target)/abs(target)*100);
    band = abs(y - target) > 0.01*abs(target);
    if any(band)
        ts = find(band, 1, 'last') * Ts;
    else
        ts = 0;
    end
end

function y = sim_dist(kp, ki, kd, imax, target, uMax, dist, Ts, Tend)
% 二阶 theta'' = u - dist（dist 模拟摩擦/重力等恒定扰动）
    N = round(Tend/Ts);
    y = 0; dy = 0; errsum = 0; prevErr = 0;
    out = zeros(1,N);
    for k = 1:N
        err = target - y;
        errsum = errsum + err;
        if isfinite(imax)
            errsum = max(-imax/ki, min(imax/ki, errsum));
        end
        der = (err - prevErr)/Ts;
        prevErr = err;
        u = kp*err + ki*errsum + kd*der;
        u = max(-uMax, min(uMax, u));
        dy = dy + Ts*(u - dist);          % ★ 恒定扰动
        y  = y  + Ts*dy;
        out(k) = y;
    end
    y = out;
end
