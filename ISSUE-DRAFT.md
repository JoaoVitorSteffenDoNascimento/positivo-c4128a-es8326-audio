# Draft — review evidence before posting

Suggested destination: https://github.com/alsa-project/alsa-ucm-conf/issues

Suggested title: Positivo C4128A-14 / ES8326: sof-essx8336 UCM compatibility investigation and internal-speaker workaround

## Hardware and software verified locally

- DMI system vendor: Positivo Tecnologia SA
- DMI product: C4128A-14
- DMI board: N14DP9R
- ACPI/I2C device: ESSX8326:00
- Bound codec driver: es8326
- Distribution: Arch Linux
- Kernel package: 7.2.9.arch1-1
- alsa-ucm-conf: 1.2.16.1-1
- alsa-lib: 1.2.16.1-1
- PipeWire: 1.6.9-1
- WirePlumber: 0.5.18-1
- SOF firmware package: 2026.09.1-1

No serial numbers, usernames, account identifiers or credentials are included here.

## Problem and scope

I am a Positivo C4128A-14 user running Arch Linux. With this local workaround, audio playback through the internal speakers is working normally, as confirmed by me on this notebook. The microphone has not been tested yet. Headphones, HDMI audio and suspend/resume have not been verified. The exact stock-configuration symptom, error messages and comparison with the workaround must still be documented before submission. This draft does not claim that all Linux distributions fail or that the kernel lacks an ES8326 driver.

The installed Intel/sof-essx8336 UCM BootSequence references Left/Right Headphone Mixer DAC switches, Headphone Playback Volume and Headphone Mixer Volume. The corresponding HiFi configuration describes the ES8316 headphone-amplifier routing. The upstream files inspected also retain these references.

This suggests a possible mismatch between the generic UCM profile and the controls exposed by ES8326 on this machine, but the running mixer's complete control list and the exact failing UCM operation are not yet attached. Kernel, firmware, machine-driver quirks and UCM causes have not been isolated completely.

## Open-source contribution and reuse

I am sharing this workaround as open source under the BSD-3-Clause license. Maintainers and community members are welcome to use, study, modify and redistribute it under that license, including adapting it into a suitable upstream fix. This is a user contribution and is not an official Positivo release.

The separate contribution package contains Python sources, tests and configuration templates without personal HOME paths. It does not include my private dotfiles, accounts, diagnostics or credentials. The templates require replacing @HOME@ with the local user's actual HOME; they are not an automatic installation package.

## Additional symptom reported by the notebook owner

On some Linux distributions, I observed audio working normally for approximately 10–15 minutes, then becoming silent until the next computer reboot. The specific distributions/versions and the exact failure timeline still need to be documented.

This workaround may be worth testing for users with the same Positivo C4128A-14 / ES8326 hardware and symptom. Internal-speaker playback with the workaround is confirmed on my notebook, but sustained testing has not yet established that it prevents this delayed audio loss, particularly across other distributions. Microphone testing remains pending. This timing-related failure has not been proven to share the suspected UCM initialization cause.

Useful follow-up evidence would include elapsed time to failure, whether playback was continuous or interrupted, whether it followed headphone/suspend events, and sanitized onboard mixer/control and relevant audio-service state before and after failure. No root cause is asserted from the timing alone.

## Local workaround, as implemented

- Match the specific onboard sof-essx8336 PipeWire device.
- Disable UCM for that device and use a custom ACP profile.
- Expose a fixed internal-speaker route rather than relying on headphone-jack state.
- Initialize Headphone Switch off and Speaker Switch on using libasound.
- Set available digital DAC/HPL/HPR/SPKL/SPKR playback controls using the driver's dB information.
- Apply initialization before WirePlumber starts.

This is a userspace workaround, not a new kernel codec driver. It deliberately restricts the route to internal speakers; headphone/HDMI behavior and microphone functionality are not validated by the current contribution draft. No application of this workaround was performed during preparation of this report.

## Evidence required before posting

- [x] Internal-speaker playback works normally with the workaround, confirmed by the notebook owner.
- [x] Record the owner-reported symptom on some distributions: audio works for about 10–15 minutes, then disappears until reboot.
- [ ] Identify the affected distributions/versions and collect the exact failure evidence.
- [ ] Test sustained playback with the workaround beyond the reported failure window and record the result.
- [ ] Attach a sanitized alsa-info report and complete onboard ALSA controls, types/ranges/dB mapping.
- [ ] Record the exact UCM parse/enable error, without changing a working setup blindly.
- [ ] Identify loaded SOF topology and board/component identifiers, omitting serial numbers.
- [ ] Test speaker playback, microphone recording, headphones, unplug/replug and suspend/resume; mark untested cases explicitly.
- [ ] Compare with upstream UCM in an isolated/reversible test; do not overwrite /usr/share/alsa automatically.
- [ ] Confirm that this is not the same hardware/problem as an existing issue.

Read-only control listing, if alsa-utils is installed, from the real terminal:

```sh
amixer -c sofessx8336 controls
amixer -c sofessx8336 contents
```

These list controls and values; they do not set mixer values. Review any diagnostic file before sharing. Avoid running alsa-info with automatic upload; collect locally and sanitize first. Avoid broad journal/system-log uploads.

## Maintainer question

Would it be appropriate to add codec/control-specific ES8326 handling to Intel/sof-essx8336 UCM, or does this board first require a machine-driver/kernel quirk? I can provide the sanitized hardware/control evidence and test a narrowly scoped proposed fix on this notebook.

## Related primary sources

- Kernel codec driver: https://github.com/torvalds/linux/blob/master/sound/soc/codecs/es8326.c
- UCM BootSequence: https://github.com/alsa-project/alsa-ucm-conf/blob/master/ucm2/Intel/sof-essx8336/sof-essx8336.conf
- UCM HiFi profile: https://github.com/alsa-project/alsa-ucm-conf/blob/master/ucm2/Intel/sof-essx8336/HiFi.conf
- UCM project and validation requirements: https://github.com/alsa-project/alsa-ucm-conf
- Related issue to compare, not assumed identical: https://github.com/alsa-project/alsa-ucm-conf/issues/407
- Kernel submission process if a kernel change is needed: https://www.kernel.org/doc/html/latest/process/submitting-patches.html
