# 0.1.4: SignalException and Interrupt are right from the first line (before anything loads the
# signal table: Signal, trap, at_exit, Dir.home, Process.kill).
e = SignalException.new("HUP")
puts e.message, e.signo
e = SignalException.new(15)
puts e.message, e.signo
e = SignalException.new(10, "custom")
puts e.message, e.signo
e = SignalException.new
puts e.message, e.signo
i = Interrupt.new
p i.message, i.signo, i.is_a?(SignalException)
p Interrupt.new("stop").message
begin
  SignalException.new("NOSUCH")
rescue ArgumentError => x
  puts x.message
end
begin
  raise Interrupt
rescue Interrupt => x
  puts "rescued #{x.class} #{x.signo}"
end
