function Get-ReleaseContentScanText {
    param([string]$RelativePath, [AllowEmptyString()][string]$Text)

    # 这两处是既有 UI 探针及其断言，不是用户凭据；诊断协议保留原名。
    # 仅在指定源码位置、完整表达式和值均匹配时替换扫描视图中的字段名。
    # 保留值和相邻文本，避免隐藏同一行的 API Key、密码或本机路径；不改文件。
    # 表达式变化不再豁免，继续由原规则报告路径和模式，不输出疑似秘密正文。
    $relative = $RelativePath.Replace('\', '/')
    $pattern = $null
    if ($relative -ieq 'native/src/launcher/api_config.c') {
        $pattern = '\bwrite_stdout_utf8\(\(GetWindowLongW\(d\.key, GWL_STYLE\) & ES_PASSWORD\) \? L"password\x3d1\\n" : L"password\x3d0\\n"\);'
    } elseif ($relative -ieq 'tests/launcher_parity/run_launcher_parity.ps1') {
        $pattern = '''\(\?m\)\^password\x3d1\$'''
    }
    if (-not $pattern) { return $Text }
    $scan = [regex]::Replace($Text, $pattern, [Text.RegularExpressions.MatchEvaluator]{
        param($match)
        $match.Value.Replace('password', 'ui_flag_')
    })
    if ($scan -cne $Text) {
        Write-Host "Release scan: recognized UI probe flags in $RelativePath"
    }
    return $scan
}
