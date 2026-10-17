% B 线环境自检：把结果同时打印并写入 check_env_out.txt（UTF-8，方便中文阅读）
% 跑法：在本目录下  matlab -batch check_env
clc;
f = fopen('check_env_out.txt', 'w', 'n', 'UTF-8');
out = @(varargin) cellfun(@(s) fprintf(f, '%s\n', s), varargin);

out(sprintf('MATLAB %s  (%s)', version, computer));

v = ver; names = {v.Name};
out(sprintf('已装过的组件数：%d', numel(names)));
for k = 1:numel(names)
    out(sprintf('  - %s %s', names{k}, v(k).Version));
end

hasSim = exist('load_system', 'file') > 0 || ~isempty(which('sim'));
out(sprintf('Simulink 可用：%s', mat2str(hasSim)));
out(sprintf('license(''test'',''Simulink'',) = %d', license('test', 'Simulink')));

models = {'simple_PID', 'single_PID_actual', 'double_PID_actual', 'd_filter_demo'};
if ~hasSim
    out('Simulink 未安装 —— 4 个 .slx 打不开，B 线改用 python 仿真（02_sim/python）。');
else
    for k = 1:numel(models)
        fname = [models{k} '.slx'];
        if ~isfile(fname), out(['MISS ' fname]); continue; end
        try
            load_system(models{k});
            blks = find_system(models{k}, 'Type', 'Block');
            out(sprintf('OK   %-22s 模块数=%d', fname, numel(blks)));
            for b = 1:min(numel(blks), 10), out(['       ' blks{b}]); end
            close_system(models{k}, 0);
        catch ME
            out(sprintf('FAIL %-22s %s', fname, ME.message));
        end
    end
end
fclose(f);
type check_env_out.txt
