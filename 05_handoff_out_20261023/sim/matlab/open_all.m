% B 线：一次性打开 4 个 PID 仿真模型（在 Simulink 里手动看波形、改参数）
cd(fileparts(mfilename('fullpath')));
for m = {'simple_PID','single_PID_actual','double_PID_actual','d_filter_demo'}
    try
        open_system(m{1});
        fprintf('已打开 %s\n', m{1});
    catch ME
        fprintf('打开失败 %s : %s\n', m{1}, ME.message);
    end
end
