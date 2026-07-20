# Terms of Use

HMS-CPAP is free and open source software licensed under the
[MIT License](LICENSE). **The MIT License is the binding legal grant.** This
document explains the terms in plain language and sets out the conditions of use
that follow from the nature of the software. Where anything here conflicts with
the LICENSE, the LICENSE governs.

By downloading, building, running, or distributing this software, you accept
these terms. If you do not accept them, do not use the software.

## 1. What you are allowed to do

Everything the MIT License permits: use it, copy it, modify it, distribute it,
and use it commercially, for free, including in closed-source products. The only
condition is that you keep the copyright notice and the license text in copies
and substantial portions of the software.

You do not need permission and you do not owe anyone a fee.

## 2. This is not a medical device

**You must not use this software as a medical device, for diagnosis, for
treatment decisions, or for the monitoring of any person whose safety depends on
that monitoring.** This is the central condition of use.

[DISCLAIMER.md](DISCLAIMER.md) is part of these terms. Read it in full.

If you deploy this software in a clinical, care, commercial, or any other setting
serving people other than yourself, you do so entirely at your own risk and take
on full legal responsibility for that deployment, including any medical device,
health privacy, and professional obligations that apply. The authors have no
involvement in and no responsibility for such use.

## 3. Your data is your responsibility

The software runs on your infrastructure and handles your health data. You are
responsible for securing it, backing it up, and complying with any law that
applies to you. See [PRIVACY.md](PRIVACY.md), which describes every way data can
leave your machine.

The software ships with no authentication and must not be exposed directly to the
internet.

## 4. Third-party services

Enabling an integration -- SleepHQ, CpapDash, an LLM provider, an MQTT broker --
means sending your data to a third party under **their** terms and **their**
privacy policy, not these. Read them. This project has no control over those
services, makes no representation about them, and takes no responsibility for
what they do with data you choose to send.

Third-party names and marks are used descriptively. See [NOTICE](NOTICE).

## 5. No warranty

The software is provided **"AS IS", WITHOUT WARRANTY OF ANY KIND**, express or
implied, including but not limited to the warranties of merchantability, fitness
for a particular purpose, accuracy, and noninfringement.

Concretely, and without limiting the above: nothing guarantees that the software
runs, that it collects your data, that it keeps collecting it, that anything it
reports is correct, or that it is compatible with your machine, your firmware, or
tomorrow's version of either. It can silently stop working. Parsers can misread
undocumented formats. Do not rely on it for anything that matters.

## 6. Limitation of liability

**IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES, OR OTHER LIABILITY**, whether in an action of contract, tort, or
otherwise, arising from, out of, or in connection with the software or its use.

This includes, without limitation: any harm to health arising from reliance on
its output, missing or incorrect data, lost or corrupted data, unauthorized
access to your data, failure of any integration, and any consequence of your
deployment of the software for other people.

Some jurisdictions do not allow the exclusion of certain warranties or
liabilities. Where that is the case, the exclusions above apply to the maximum
extent permitted by law.

## 7. Contributions

Contributions are welcome. By submitting one, you confirm that you wrote it or
otherwise have the right to submit it, and you license it under the MIT License
on the same terms as the rest of the project.

**Do not contribute code copied or derived from GPL-licensed projects, including
OSCAR, or from any manufacturer's proprietary software.** Format facts, offsets,
and independently written parsers are fine; copied implementations are not. See
[NOTICE](NOTICE) section 2.

Do not include real patient data, credentials, tokens, or private network details
in issues, pull requests, sample files, or test fixtures.

## 8. No support

There is no obligation to provide support, fixes, updates, or continued
development, and no service level of any kind. Issues and pull requests may go
unanswered. The project may be changed or abandoned at any time without notice.

## 9. Changes to these terms

These terms may change in future releases. Changes apply to the versions that
carry them; they do not retroactively alter the license of a version you already
received. The MIT grant on any released version is irrevocable.
