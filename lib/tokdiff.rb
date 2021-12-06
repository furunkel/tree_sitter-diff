# frozen_string_literal: true

require_relative 'tokdiff/version'
require_relative 'tokdiff/core'

module Tokdiff
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
