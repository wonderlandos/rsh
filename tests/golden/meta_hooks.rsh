# SPDX-License-Identifier: GPL-2.0-or-later
# Module hooks (included, extended, prepended, inherited, method_added), prepend around a method
# that runs a command, const_missing, ObjectSpace.
module Logged
  def self.prepended(base)
    puts "prepended to #{base}"
  end

  def deploy(target)
    puts "before #{target}"
    r = super
    puts "after #{target}: #{r}"
    r
  end
end

module Helpers
  def self.included(base)
    puts "included in #{base}"
    base.extend(ClassMethods)
  end

  module ClassMethods
    def self.extended(base)
      puts "extended #{base}"
    end

    def steps(*names)
      @steps = names
    end

    def step_list = @steps
  end

  def shout(s) = s.upcase
end

class Deployer
  include Helpers
  prepend Logged
  steps :build, :copy

  def self.inherited(sub)
    super
    puts "inherited by #{sub}"
  end

  def self.method_added(name)
    puts "method_added #{name}" if name == :deploy
  end

  def deploy(target)
    echo deploying #{shout(target)}
  end
end

class Staging < Deployer; end
p Deployer.ancestors.take(3), Deployer.step_list
p Staging.new.deploy("a b")
p Deployer.include?(Helpers), Deployer.singleton_class.include?(Helpers::ClassMethods)

module Settings
  DEFAULTS = {"Port" => 22}
  def self.const_missing(name)
    DEFAULTS.fetch(name.to_s) { super }
  end
end
p Settings::Port
begin
  Settings::Nope
rescue NameError => e
  puts "NameError #{e.name}"
end

subs = []
ObjectSpace.each_object(Class) { |c| subs << c.name if c.superclass == Deployer }
p subs
p ObjectSpace.count_objects.key?(:TOTAL)
