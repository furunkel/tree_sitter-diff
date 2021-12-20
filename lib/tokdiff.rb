# frozen_string_literal: true

require_relative 'tokdiff/version'
require_relative 'tokdiff/core'

module Tokdiff

  def self.diff(old, new, output_equal: false, ignore_whitespace: true)
    __diff__ old, new, output_equal, ignore_whitespace
  end

  def self.tokenize(input, ignore_whitespace: true)
    __tokenize__ input, ignore_whitespace
  end

  class Token
    def inspect
      %Q(#<#{self.class} "#{text}">)
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
