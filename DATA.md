  > Read CLAUDE.md, SESSION.md, STATUS.md, PLAN.md, and the current protocol inventory.
  >
  > Strategic priority change: formally pause active gyro investigation.
  >
  > Preserve these as documented future paths, but do not execute them now:
  >
  > - BLE 40-byte-block decoding
  > - Windows-only HID report-0x09 capture
  > - BLE→Windows→Wi-Fi→Pico experimental relay
  > - Dycool/Usb-relay-for-NS repair
  > - Report-0x09 filters, transforms, integration, or encoder changes
  >
  > Gyro is not abandoned; it is deferred until broader controller reverse engineering or new
  > primary evidence changes the situation.
  >
  > Shift to systematic reverse engineering of the remaining genuine Switch 2 Pro Controller
  > features. Use NFC/amiibo as the first bounded subsystem.
  >
  > Nintendo’s official specifications confirm that the Switch 2 Pro Controller supports NFC and
  > has an NFC touchpoint. Treat feature existence as confirmed, but treat its protocol,
  > hardware, initialization, and compatibility with Switch 1 NFC as unknown until supported by
  > evidence.
  >
  > Your only objective this turn is to establish the current state of Switch 2 controller NFC
  > reverse engineering and identify the smallest experiment needed to advance it.
  >
  > Search current primary technical sources and implementations, including:
  >
  > - Switch 2 Pro Controller and Joy-Con 2 research repositories
  > - Open-source Switch 2 controller drivers and bridges
  > - Switchbrew documentation
  > - Ndeadly’s research and tools
  > - Joy-Con 2 NFC implementations
  > - Switch 1 controller/NFC research only as a comparison baseline
  > - This repository’s USB/BLE captures, commands, descriptors, GATT discovery, SPI data, and
  >   unknown report regions
  >
  > Do not provide broad project summaries. Extract exact actionable evidence:
  >
  > - Repository, branch, file, function, and commit where relevant
  > - NFC IC identification and confidence
  > - GATT characteristics, USB interfaces, endpoints, handles, report IDs, and lengths
  > - Feature-enable bits or commands
  > - Initialization and polling sequences
  > - NFC state-machine transitions
  > - Tag-detection, read, write, mount, and unmount operations
  > - Checksums, encryption, authentication, and framing
  > - Timing requirements
  > - Captures or test utilities that can be reused
  > - Which findings concern Joy-Con 2 versus Pro Controller 2
  >
  > Keep these claims separate:
  >
  > 1. Official confirmation that the Pro Controller 2 contains NFC.
  > 2. Physical NFC hardware identified in Joy-Con 2.
  > 3. Physical NFC hardware identified in the Pro Controller 2.
  > 4. Protocol behavior demonstrated on Joy-Con 2.
  > 5. Protocol behavior demonstrated on the Pro Controller 2.
  > 6. Switch 1 behavior that might—but has not yet been shown to—carry forward.
  >
  > Do not transfer a Joy-Con 2 or Switch 1 implementation to the Pro Controller 2 merely
  > because Nintendo may reuse components or command concepts.
  >
  > Inspect this repository for dormant NFC evidence:
  >
  > - Unknown commands already exchanged during genuine initialization
  > - Unmapped feature-mask bits
  > - Unexplained GATT characteristics discovered in the live service map
  > - USB interfaces or endpoints not used by normal input
  > - NFC-related strings, constants, descriptors, or command IDs
  > - Relevant regions in existing captures
  >
  > Produce a confidence-qualified NFC protocol inventory containing:
  >
  > - Confirmed
  > - Strong evidence
  > - Hypotheses
  > - Unknowns
  > - Conflicts between sources
  > - Evidence specific to each controller type and transport
  >
  > Then choose one next step:
  >
  > 1. If an existing Pro Controller 2 NFC implementation or capture exists, reproduce its exact
  >    protocol map offline and identify what this repository can validate.
  >
  > 2. If only Joy-Con 2 evidence exists, design a minimal genuine Pro Controller 2 capture
  >    experiment that determines whether the same commands, handles, or reports appear.
  >
  > 3. If no actionable protocol evidence exists, identify the best observation point and
  >    required hardware/software before implementing anything.
  >
  > Do not implement NFC emulation this turn. Do not send exploratory writes to unknown handles.
  > Do not modify normal controller behavior. Analysis, reusable read-only tooling, and
  > documentation only.
  >
  > Update STATUS.md, PLAN.md, and the appropriate controller-protocol documentation. Mark gyro
  > as paused for new evidence rather than still presenting it as the immediate objective.
  >
  > End with:
  >
  > - The strongest existing Pro Controller 2 NFC evidence
  > - The most important unsupported assumption
  > - One exact next capture or analysis task
  > - Why that task has higher information value than implementing NFC from Switch 1 assumptions