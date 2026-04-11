#!/usr/bin/env python3
"""
Falcon 下载器 - 测试报告生成器
从测试XML文件生成HTML格式的测试报告
"""

import os
import sys
import xml.etree.ElementTree as ET
from datetime import datetime
import glob

def parse_xml_file(xml_file):
    """解析单个测试XML文件"""
    try:
        tree = ET.parse(xml_file)
        root = tree.getroot()

        if root.tag == 'testsuite':
            test_suite = root
        else:
            test_suite = root.find('testsuite')
            if test_suite is None:
                suites = root.findall('testsuite')
                if suites:
                    aggregated = {
                        'name': os.path.basename(xml_file),
                        'tests': 0,
                        'failures': 0,
                        'errors': 0,
                        'time': 0.0,
                        'test_cases': []
                    }
                    for suite in suites:
                        aggregated['tests'] += int(suite.get('tests', 0))
                        aggregated['failures'] += int(suite.get('failures', 0))
                        aggregated['errors'] += int(suite.get('errors', 0))
                        aggregated['time'] += float(suite.get('time', 0))
                        for test_case in suite.findall('testcase'):
                            status = 'PASS'
                            failure_msg = ''

                            failure = test_case.find('failure')
                            if failure is not None:
                                status = 'FAIL'
                                failure_msg = failure.get('message', '')

                            error = test_case.find('error')
                            if error is not None:
                                status = 'ERROR'
                                failure_msg = error.get('message', '')

                            aggregated['test_cases'].append({
                                'name': test_case.get('name', ''),
                                'classname': test_case.get('classname', ''),
                                'time': float(test_case.get('time', 0)),
                                'status': status,
                                'message': failure_msg
                            })

                    return aggregated

                return None

        tests = int(test_suite.get('tests', 0))
        failures = int(test_suite.get('failures', 0))
        errors = int(test_suite.get('errors', 0))
        time = float(test_suite.get('time', 0))

        test_cases = []
        for test_case in test_suite.findall('testcase'):
            name = test_case.get('name', '')
            classname = test_case.get('classname', '')
            time = float(test_case.get('time', 0))

            status = 'PASS'
            failure_msg = ''
            failure = test_case.find('failure')
            if failure is not None:
                status = 'FAIL'
                failure_msg = failure.get('message', '')

            error = test_case.find('error')
            if error is not None:
                status = 'ERROR'
                failure_msg = error.get('message', '')

            test_cases.append({
                'name': name,
                'classname': classname,
                'time': time,
                'status': status,
                'message': failure_msg
            })

        return {
            'name': test_suite.get('name', os.path.basename(xml_file)),
            'tests': tests,
            'failures': failures,
            'errors': errors,
            'time': time,
            'test_cases': test_cases
        }
    except Exception as e:
        print(f"Error parsing {xml_file}: {e}")
        return None

def generate_html_report(results_dir, output_file):
    """生成HTML测试报告"""

    # 查找所有XML文件
    xml_files = glob.glob(os.path.join(results_dir, '*.xml'))
    if not xml_files:
        print("No XML files found in", results_dir)
        return

    # 解析所有测试结果
    all_results = []
    total_tests = 0
    total_failures = 0
    total_errors = 0
    total_time = 0.0

    for xml_file in xml_files:
        result = parse_xml_file(xml_file)
        if result:
            all_results.append(result)
            total_tests += result['tests']
            total_failures += result['failures']
            total_errors += result['errors']
            total_time += result['time']

    # 生成HTML报告
    html_content = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Falcon 下载器 - 测试报告</title>
    <style>
        body {{
            font-family: Arial, sans-serif;
            margin: 0;
            padding: 20px;
            background-color: #f5f5f5;
        }}
        .container {{
            max-width: 1200px;
            margin: 0 auto;
            background-color: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        h1 {{
            color: #333;
            border-bottom: 2px solid #4CAF50;
            padding-bottom: 10px;
        }}
        .summary {{
            display: flex;
            justify-content: space-between;
            margin: 20px 0;
            padding: 15px;
            background-color: #f9f9f9;
            border-radius: 5px;
        }}
        .summary-item {{
            text-align: center;
        }}
        .summary-value {{
            font-size: 24px;
            font-weight: bold;
        }}
        .summary-label {{
            color: #666;
            font-size: 14px;
        }}
        .pass {{ color: #4CAF50; }}
        .fail {{ color: #f44336; }}
        .error {{ color: #ff9800; }}

        table {{
            width: 100%;
            border-collapse: collapse;
            margin: 20px 0;
        }}
        th, td {{
            padding: 12px;
            text-align: left;
            border-bottom: 1px solid #ddd;
        }}
        th {{
            background-color: #4CAF50;
            color: white;
        }}
        tr:hover {{
            background-color: #f5f5f5;
        }}
        .status-pass {{ color: #4CAF50; font-weight: bold; }}
        .status-fail {{ color: #f44336; font-weight: bold; }}
        .status-error {{ color: #ff9800; font-weight: bold; }}

        .test-details {{
            margin-top: 20px;
        }}
        .test-case {{
            margin: 10px 0;
            padding: 10px;
            border-left: 4px solid #ddd;
            background-color: #fafafa;
        }}
        .test-case.fail {{
            border-left-color: #f44336;
            background-color: #ffebee;
        }}
        .test-case.error {{
            border-left-color: #ff9800;
            background-color: #fff3e0;
        }}

        .timestamp {{
            color: #999;
            font-size: 12px;
            text-align: right;
            margin-top: 20px;
        }}
    </style>
</head>
<body>
    <div class="container">
        <h1>Falcon 下载器 - 测试报告</h1>

        <div class="summary">
            <div class="summary-item">
                <div class="summary-value">{total_tests}</div>
                <div class="summary-label">总测试数</div>
            </div>
            <div class="summary-item">
                <div class="summary-value pass">{total_tests - total_failures - total_errors}</div>
                <div class="summary-label">通过</div>
            </div>
            <div class="summary-item">
                <div class="summary-value fail">{total_failures}</div>
                <div class="summary-label">失败</div>
            </div>
            <div class="summary-item">
                <div class="summary-value error">{total_errors}</div>
                <div class="summary-label">错误</div>
            </div>
            <div class="summary-item">
                <div class="summary-value">{total_time:.2f}s</div>
                <div class="summary-label">总耗时</div>
            </div>
        </div>

        <h2>测试套件详情</h2>
        <table>
            <thead>
                <tr>
                    <th>测试套件</th>
                    <th>测试数</th>
                    <th>通过</th>
                    <th>失败</th>
                    <th>错误</th>
                    <th>耗时</th>
                </tr>
            </thead>
            <tbody>"""

    for result in all_results:
        passed = result['tests'] - result['failures'] - result['errors']
        html_content += f"""
                <tr>
                    <td>{result['name']}</td>
                    <td>{result['tests']}</td>
                    <td class="pass">{passed}</td>
                    <td class="fail">{result['failures']}</td>
                    <td class="error">{result['errors']}</td>
                    <td>{result['time']:.2f}s</td>
                </tr>"""

    html_content += """
            </tbody>
        </table>

        <h2>测试详情</h2>
        <div class="test-details">"""

    # 添加失败的测试详情
    for result in all_results:
        if result['failures'] > 0 or result['errors'] > 0:
            html_content += f"<h3>{result['name']}</h3>"
            for test_case in result['test_cases']:
                if test_case['status'] != 'PASS':
                    css_class = "fail" if test_case['status'] == 'FAIL' else "error"
                    html_content += f"""
                    <div class="test-case {css_class}">
                        <div><strong>测试:</strong> {test_case['classname']}.{test_case['name']}</div>
                        <div><strong>状态:</strong> <span class="status-{css_class}">{test_case['status']}</span></div>
                        <div><strong>耗时:</strong> {test_case['time']:.3f}s</div>
                        {f'<div><strong>错误信息:</strong> <pre>{test_case["message"]}</pre></div>' if test_case['message'] else ''}
                    </div>"""

    html_content += f"""
        </div>

        <div class="timestamp">
            报告生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
        </div>
    </div>
</body>
</html>"""

    # 写入HTML文件
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(html_content)

    print(f"HTML报告已生成: {output_file}")

def main():
    if len(sys.argv) != 2:
        print("用法: python generate_test_report.py <测试结果目录>")
        sys.exit(1)

    results_dir = sys.argv[1]
    if not os.path.isdir(results_dir):
        print(f"错误: 目录不存在 - {results_dir}")
        sys.exit(1)

    output_file = os.path.join(results_dir, '..', 'test_reports', 'test_report.html')
    os.makedirs(os.path.dirname(output_file), exist_ok=True)

    generate_html_report(results_dir, output_file)

if __name__ == '__main__':
    main()
