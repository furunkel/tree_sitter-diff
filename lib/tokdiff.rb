# frozen_string_literal: true

require_relative 'tokdiff/version'
require_relative 'tokdiff/core'

module Tokdiff

  def self.diff(language, old, new, output_equal: false, ignore_whitespace: true, ignore_comments: false, split_lines: false)
    __diff__ language.to_sym, old, new, output_equal, ignore_whitespace, ignore_comments, split_lines
  end

  def self.tokenize(language, input, ignore_whitespace: true, ignore_comments: false)
    __tokenize__ language.to_sym, input, ignore_whitespace, ignore_comments
  end

  class Token
    def inspect
      %Q(#<#{self.class} "#{text}" #{byte_range}>)
    end
  end

  class ChangeSet
    def inspect
      peek_size = 10
      peek = self.each.take([peek_size, size].min).map(&:inspect).join(', ')
      "#<#{self.class} #{type} [#{peek}#{size > peek_size ? ', ...' : ''}]>"
    end
  end
end
