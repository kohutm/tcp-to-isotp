#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import sys

from github import Github

# List of labels that indicate a PR should not be merged
# These labels are considered blocking and will prevent merging
DNM_LABELS = ["DNM", "DNM (manifest)", "TSC", "Architecture Review", "dev-review"]


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Prevent merging of PRs with DNM labels or missing metadata.",
        allow_abbrev=False,
    )
    parser.add_argument("-p", "--pull-request", required=True, type=int, help="The PR number")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)

    token = os.environ.get("GITHUB_TOKEN")
    repo_name = os.environ.get("GITHUB_REPOSITORY")

    if not token or not repo_name:
        print("❌ Missing GITHUB_TOKEN or GITHUB_REPOSITORY in environment.")
        sys.exit(1)

    gh = Github(token)

    try:
        repo = gh.get_repo(repo_name)
        pr = repo.get_pull(args.pull_request)
    except Exception as e:
        print(f"❌ Failed to get PR #{args.pull_request} from {repo_name}: {e}")
        sys.exit(1)

    print(f"✅ Checking PR: {pr.html_url}")

    fail = False

    # Check DNM labels
    for label in pr.get_labels():
        print(f"ℹ️ label: {label.name}")
        if label.name in DNM_LABELS:
            print(f"❌ PR has blocking label: \"{label.name}\".")
            fail = True

    # Check description
    if not pr.body or not pr.body.strip():
        print("❌ PR has no description.")
        fail = True

    if fail:
        print("🚫 This pull request cannot be merged.")
        sys.exit(1)

    print("✅ PR metadata check passed successfully.")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
