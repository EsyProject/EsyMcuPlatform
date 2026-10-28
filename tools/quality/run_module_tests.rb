#!/usr/bin/env ruby
# SPDX-FileCopyrightText: 2026 EsyProject contributors
#
# SPDX-License-Identifier: Apache-2.0

# Run a complete Ceedling configuration with an explicit artifact directory.
# Ceedling 1.0.1 mixins do not reliably replace project.build_root.
require 'yaml'
require 'tmpdir'
require 'tempfile'
require 'fileutils'
require 'json'
require 'open3'

unless (3..4).cover?(ARGV.length)
  abort 'usage: run_module_tests.rb TEST_DIRECTORY OUTPUT_DIRECTORY TASK [--sanitizer|--mixin=NAME.yml]'
end
source = File.expand_path(ARGV[0])
output = File.expand_path(ARGV[1])
task = ARGV[2]
abort 'invalid test task' unless task.match?(/\A(?:test:(?:all|test_[A-Za-z0-9_]+)|gcov:all)\z/)
option = ARGV[3]
mixin = option&.match(/\A--mixin=([a-z0-9_]+\.yml)\z/)
abort 'invalid option' if option && option != '--sanitizer' && !mixin
cache_helper = File.expand_path('../providers/provider_cache.py', __dir__)
provider_json, provider_errors, provider_status = Open3.capture3('python3', '-B', cache_helper, '--json')
abort provider_errors unless provider_status.success?
child_environment = ENV.to_h.merge(JSON.parse(provider_json))
config = YAML.unsafe_load_file(File.join(source, 'project.yml'))
if mixin
  # Match the additive hash/array overlays used by module test variants.
  merge = lambda do |base, overlay|
    overlay.each do |key, value|
      base[key] = if base[key].is_a?(Hash) && value.is_a?(Hash)
                    merge.call(base[key], value)
                  elsif base[key].is_a?(Array) && value.is_a?(Array)
                    (base[key] + value).uniq
                  else
                    value
                  end
    end
    base
  end
  merge.call(config, YAML.unsafe_load_file(File.join(source, mixin[1])))
end
# Keep the suite's working directory and path syntax, including +:/-: entries.
config[:project][:build_root] = output
if option == '--sanitizer'
  sanitizer = YAML.unsafe_load_file(File.join(source, 'sanitizer.yml'))
  config[:flags] = sanitizer.fetch(:flags)
  sanitizer.fetch(:defines, {}).each do |scope, values|
    config[:defines] ||= {}
    config[:defines][scope] = (Array(config[:defines][scope]) + Array(values)).uniq
  end
end
# Resolve prepared software-provider paths without evaluating inline Ruby.
resolve_provider_path = lambda do |value|
  case value
  when Hash
    value.transform_values { |entry| resolve_provider_path.call(entry) }
  when Array
    value.map { |entry| resolve_provider_path.call(entry) }
  when String
    resolved = value
    %w[ESY_COREMQTT_PATH ESY_MBEDTLS_PATH ESY_LLHTTP_PATH ESY_COREJSON_PATH].each do |variable|
      placeholder = "@#{variable}@"
      next unless resolved.include?(placeholder)
      provider_path = child_environment.fetch(variable) do
        abort "error: #{variable} is required for this test provider; run ./setup.sh"
      end
      abort "error: #{variable} must not be empty" if provider_path.empty?
      resolved = resolved.gsub(placeholder) { provider_path }
    end
    resolved
  else
    value
  end
end
config = resolve_provider_path.call(config)
FileUtils.mkdir_p(output)
coverage_file = nil
begin
  gcovr = config.dig(:gcov, :gcovr)
  if task == 'gcov:all' && gcovr
    gcovr[:exception_on_fail] = true
    objects = File.join(output, 'gcov')
    if gcovr[:config_file]
      original = File.expand_path(gcovr[:config_file], source)
      # Preserve config-relative filters/options by retaining their directory.
      # Only object discovery changes. Never modify the original config file.
      coverage_file = Tempfile.new(['.esy-gcovr-', '.cfg'], File.dirname(original))
      lines = File.readlines(original).reject { |line| line.match?(/^\s*search-path\s*=/) }
      coverage_file.write(lines.join)
      coverage_file.write("\nsearch-path = #{objects}\n")
      unless lines.any? { |line| line.match?(/^\s*object-directory\s*=/) }
        coverage_file.write("object-directory = #{source}\n")
      end
      coverage_file.flush
      gcovr[:config_file] = coverage_file.path
    else
      # Append to the plugin's native tool rather than replacing its
      # version-specific argument template or report options.
      shortcut = config[:tools_gcov_gcovr_report] ||= {}
      shortcut[:arguments] = Array(shortcut[:arguments]) + ["\"#{objects}\""]
      gcovr[:object_directory] ||= source
    end
  end
  success = Dir.mktmpdir('esy-ceedling-') do |directory|
    project = File.join(directory, 'project.yml')
    File.write(project, YAML.dump(config))
    system(child_environment, 'ceedling', '--project', project, task, chdir: source)
  end
ensure
  coverage_file&.close!
end
exit(success ? 0 : 1)
