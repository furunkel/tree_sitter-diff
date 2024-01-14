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
  ext.lib_dir = 'lib/tree_sitter/diff'
end

def ext_path(filename)
  File.join('ext', 'core', filename)
end

#file ext_path('tokenizer.c') => ext_path('tokenizer.re') do |t|
#  sh "re2c #{t.prerequisites.join ' '} -o #{t.name}"
#end

#Rake::Task[:compile].enhance [ext_path('tokenizer.c'), ext_path('tokenizer.re')]

task :console do
  exec "irb -I lib -r tree_sitter/diff"
end
