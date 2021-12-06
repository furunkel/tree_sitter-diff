# frozen_string_literal: true

require "bundler/gem_tasks"
require "rake/testtask"

Rake::TestTask.new(:test) do |t|
  t.libs << "test"
  t.libs << "lib"
  t.test_files = FileList["test/**/*_test.rb"]
end

require "rubocop/rake_task"

RuboCop::RakeTask.new

task default: %i[test rubocop]

require "rake/extensiontask"
Rake::ExtensionTask.new("core") do |ext|
  ext.lib_dir = 'lib/tokdiff'
end

task :console do
  exec "irb -I lib -r tokdiff"
end
