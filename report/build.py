"""Build the acmart report while keeping every generated file in .cache/report."""

from pathlib import Path
import os
import shutil
import subprocess


def main():
    source = Path(__file__).resolve().parent
    output = source.parent / ".cache" / "report"
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["BIBINPUTS"] = str(source) + os.pathsep + env.get("BIBINPUTS", "")
    tex = ["xelatex", "-interaction=nonstopmode", "-halt-on-error", "-file-line-error",
           f"-output-directory={output}", "main.tex"]
    subprocess.run(tex, cwd=source, env=env, check=True)
    subprocess.run(["bibtex", "main"], cwd=output, env=env, check=True)
    for _ in range(2):
        subprocess.run(tex, cwd=source, env=env, check=True)
    cached_pdf = output / "main.pdf"
    deliverable = source / "main.pdf"
    shutil.copyfile(cached_pdf, deliverable)
    print(f"Report: {deliverable}")


if __name__ == "__main__":
    main()
