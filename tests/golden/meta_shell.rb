#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# A .rb file has the shell layer: metaprogramming and command lines in the same file.
module Steps
  def self.extended(base) = (base.instance_variable_set(:@steps, []))

  def step(name, &body)
    @steps << name
    define_method(name, &body)
  end

  def steps = @steps
end

class Release
  extend Steps
  rsh_methods :fetch, :pack

  step :fetch do
    echo fetching #{version}
  end

  step :pack do
    echo "packing #{version}" | tr a-z A-Z
  end

  attr_reader :version

  def initialize(version) = (@version = version)

  def run
    fetch
    pack
    self.class.steps.size
  end
end

p Release.steps
p Release.new("1.0 beta").run
p Release.instance_method(:fetch).arity
