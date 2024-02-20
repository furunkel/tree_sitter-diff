# frozen_string_literal: true

require 'tree_sitter'
require_relative 'diff/version'
require_relative 'diff/core'

module TreeSitter
  module Diff
    def self.diff(old, new, output_equal: false, output_replace: false, ignore_whitespace: true, ignore_comments: false)
      __diff__ old, new, output_equal, output_replace, ignore_whitespace, ignore_comments
    end

    class ChangeSet
      def inspect
        peek_size = 10
        peek = self.each.take([peek_size, size].min).map(&:inspect).join(', ')
        "#<#{self.class} #{type} [#{peek}#{size > peek_size ? ', ...' : ''}]>"
      end

      def pq_profile(p, q, profile = nil, include_root_ancestors: true, raw: false, max_depth: 3)
        __pq_profile__(p, q, profile, include_root_ancestors, raw, max_depth)
      end
    end
  end
end
