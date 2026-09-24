Put your course material in this directory.

Files ending in .txt .md .R .Rmd or .csv are read; anything else is
ignored. Save them as UTF-8. Plain text and Markdown work best; a PDF or
a .docx has to be exported to text first.

What is worth putting here:

  - the assignment brief and the marking criteria
  - the lecturer's notes on the methods the course uses
  - worked examples with the code and the expected output
  - the house style: how results should be reported, how many decimal
    places, which plots, which packages are allowed
  - a list of the datasets used, with the output of str() for each

What is not worth putting here:

  - whole textbooks; only a few thousand words reach the model per
    question and long files crowd out the useful ones
  - raw data files, unless small and needed to explain a structure

How the selection works. Every question triggers a fresh scan of this
directory. If everything fits inside context_max_chars (see
R_HOME/etc/Rai.conf) then everything is sent. If it does not, each file
is scored by how many words of four or more characters it shares with
your question, and the highest scoring files are sent until the budget
runs out. So giving each file a descriptive name and keeping one topic
per file makes the selection noticeably better.

A practical layout:

  01-assignment-brief.md
  02-reporting-style.md
  03-descriptive-statistics.md
  04-t-tests.md
  05-anova.md
  06-regression.md
  07-worked-example-lab3.R

There is no index to rebuild and nothing to restart: edit a file, ask
the next question, and the new text is used.
