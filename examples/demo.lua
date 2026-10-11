-- Script for examples/demo.sh. Declarations here become named signals,
-- generators, and synths that clips and expressions refer to.

-- 0..1 envelope that peaks on each beat at 120 BPM.
mf.signal("pulse", function()
  local phase = fmod(t * 2, 1)
  return exp(-phase * 6)
end)

-- Bars whose heights follow noise and the beat.
mf.generator("bars", function(g)
  local n = 24
  local bw = g.w / n
  for i = 0, n - 1 do
    local level = (0.5 + 0.4 * noise(i * 0.7 + g.t * 1.5)) * (0.6 + 0.4 * sig.pulse)
    local h = g.h * 0.5 * level
    g:rect(i * bw + 2, g.h - h, bw - 4, h, hsv(0.55 + i / n * 0.2, 0.5, 0.9))
  end
end)

-- A soft two-oscillator tone, gated by the beat.
mf.synth("tone", function(time)
  local env = exp(-fmod(time * 2, 1) * 5)
  return (sin(time * 2 * pi * 110) + 0.5 * sin(time * 2 * pi * 165)) * 0.3 * env
end)
