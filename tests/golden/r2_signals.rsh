# 0.1.4 (audit H-2, M-6): at_exit, trap, Signal, Process.kill with names.
at_exit { puts "at_exit 1 (runs last)" }
at_exit { puts "at_exit 2 (runs first)" }
trap("USR1") { puts "trap USR1 ran" }
Process.kill("USR1", Process.pid)
sleep 0.05
trap(:USR2) { |signo| puts "trap USR2 got #{signo}" }
Process.kill(:SIGUSR2, Process.pid)
sh -c 'true'
p Signal.list["TERM"], Signal.list["ALRM"]
begin
  Process.kill("NOSUCH", Process.pid)
rescue ArgumentError => e
  puts e.message
end
p trap("USR1", "DEFAULT").is_a?(Proc)
trap("HUP", "IGNORE")
Process.kill("HUP", Process.pid)
puts "HUP ignored"
begin
  raise SignalException, "TERM"
rescue SignalException => e
  puts "#{e.message} #{e.signo}"
end
p Interrupt.new.is_a?(SignalException)
trap("EXIT") { puts "trap EXIT runs after at_exit" }
at_exit { exit 4 }
