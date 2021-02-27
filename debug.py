from pathlib import Path

import os
import sys

script_dir = Path(os.path.realpath(__file__)).parent
sys.path.append(str(script_dir))

import spacy
nlp = spacy.load("en_core_web_sm")

import vectorian
vectorian.compile_for_debugging()

from vectorian.importers import NovelImporter
from vectorian.embeddings import FastText
from vectorian.session import Session

embedding = FastText("en")

im = NovelImporter(nlp)
doc = im("/Users/arbeit/Wise Children.txt")

session = Session(
    [doc],
    [embedding])

index = session.index_for_metric()
query = nlp("large")
r = index.find(query)

print(r)