# Medical Disclaimer

**HMS-CPAP is not a medical device. Do not use it to make medical decisions.**

Read this before using the software or anything it produces.

## What this software is

HMS-CPAP is a hobbyist, self-hosted tool that reads data files written by CPAP
machines and displays them. It is a data viewer. Nothing more.

## What this software is not

- **It is not a medical device.** It has not been cleared, approved, certified,
  or reviewed by the FDA, the EMA, the MHRA, Health Canada, the TGA, or any
  other regulatory body. It carries no CE mark. It has not been validated to any
  clinical standard.
- **It is not a diagnostic tool.** It cannot diagnose sleep apnea or any other
  condition, and it cannot tell you whether your therapy is working.
- **It is not a monitoring or alarm system.** It is not designed for, and must
  not be used for, real-time monitoring of anyone whose safety depends on being
  monitored. It can silently fail to collect data, misreport a value, or stop
  running entirely, and it will not alert you when it does.
- **It is not a substitute for your clinician**, your sleep physician, your
  equipment supplier, or your machine's own labeling.

## Do not change your therapy based on this software

Pressure settings, ramp, EPR, humidity, mask type, and replacement schedules are
prescribed. **Do not change any of them because of a number or chart you saw
here.** Talk to the clinician who manages your therapy.

If you feel unwell, if your therapy feels wrong, or if you suspect your machine
is malfunctioning, contact a healthcare professional. In an emergency, contact
emergency services. Do not spend time in a dashboard.

## The numbers can be wrong

This software parses undocumented, reverse-engineered binary formats written by
machines it was never designed alongside. Correctness is a goal, not a promise.

Specific and known ways the output can mislead you:

- A parser may misread a field on a firmware revision it has never seen.
- A session may be missed, truncated, duplicated, or merged with another.
- A metric may be derived using an interpretation that differs from your
  manufacturer's own software, so the same night can legitimately produce
  different AHI values in different tools.
- Data collection may stop for days without any visible sign, so an absent
  problem may mean a broken collector rather than a good night.
- Supply and replacement reminders are arithmetic on a date you typed in. They
  are convenience timers, not clinical guidance, and they know nothing about the
  actual condition of your equipment.
- Any AI or LLM generated summary is machine generated text. It may be wrong,
  it may be confidently wrong, and it has no clinical training whatsoever.

Treat anything this software tells you as a hint to ask a better question, never
as an answer.

## Reports are not clinical records

Generated PDFs and exports are for personal reference and for sharing informally
with your own clinician if you choose. They are not a clinical record, they are
not a validated diagnostic report, and they should not be represented as either.

## If you deploy this for anyone else

Running this for another person, in a clinic, in a care setting, or as any kind
of service is entirely at your own risk and your own legal responsibility. You
become responsible for every applicable obligation, which may include HIPAA in
the United States, the GDPR in the EU and UK, medical device regulation, and
professional duties of care. This project provides no compliance of any kind and
its authors take no responsibility for such use.

## No warranty

This software is provided "as is", without warranty of any kind. See
[LICENSE](LICENSE) for the full terms and [TERMS.md](TERMS.md) for the terms of
use. Nothing in this project creates a doctor-patient relationship, and nothing
in it is medical advice.

## Independence

HMS-CPAP is an independent project. It is not affiliated with, endorsed by,
sponsored by, or supported by any CPAP manufacturer or any other company named
anywhere in this repository. See [NOTICE](NOTICE).
