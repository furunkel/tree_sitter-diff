# frozen_string_literal: true

require_relative "lib/tree_sitter/diff/version"

Gem::Specification.new do |spec|
  spec.name = "tree_sitter-diff"
  spec.version = Tokdiff::VERSION
  spec.authors = ["furunkel"]
  spec.email = ["furunkel@polyadic.com"]

  spec.summary = "..."
  spec.description = "..."
  spec.homepage = "http://example.org"
  spec.required_ruby_version = ">= 2.6.0"

  spec.metadata["allowed_push_host"] = "rubygems.org"

  spec.metadata["homepage_uri"] = spec.homepage
  spec.metadata["source_code_uri"] = "http://example.org"
  spec.metadata["changelog_uri"] = "http://example.org"

  # Specify which files should be added to the gem when it is released.
  # The `git ls-files -z` loads the files in the RubyGem that have been added into git.
  spec.files = Dir.chdir(File.expand_path(__dir__)) do
    `git ls-files -z`.split("\x0").reject do |f|
      (f == __FILE__) || f.match(%r{\A(?:(?:test|spec|features)/|\.(?:git|travis|circleci)|appveyor)})
    end
  end
  spec.bindir = "exe"
  spec.executables = spec.files.grep(%r{\Aexe/}) { |f| File.basename(f) }
  spec.require_paths = ["lib"]

  spec.add_dependency "tree_sitter"
  spec.add_development_dependency "rake-compiler"
end
