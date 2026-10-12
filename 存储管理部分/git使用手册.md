**git语句分析**

```
1. git config --global user.name "Your Name" 
作用：设置 Git 的全局用户名。`

2. git config --global user.email "email@example.com" 作用：设置 Git 的全局用户邮箱。

3. git init 
作用：在当前目录下创建一个新的 Git 仓库。

4. git add . 
作用：将当前目录下的所有文件（包括子目录）添加到 Git 的暂存区（Staging Area）。

5. git commit -m "Initial commit" 
作用：将暂存区中的更改保存为一次提交（commit），并附上一条提交信息。

6. git status 
作用：查看当前仓库状态。

7. git status -s 
作用：查看简洁格式的状态信息。

8. git diff readme.txt 
作用：readme.txt 的未暂存更改。

9. git diff --staged readme.txt 
作用：readme.txt 的已暂存更改。

10. git diff HEAD readme.txt 
作用：readme.txt 的所有本地更改。

11. git log 
作用：查看当前分支的提交历史记录（commit history）。

12. git reflog 
作用：记录本地仓库中 HEAD 的每一次移动记录，包括提交、切换分支、重置等操作。
HEAD 作用：HEAD 是一个指向当前所在提交或分支的“指针”。

13. git reset --hard commit_id 
作用：将当前分支的 HEAD、暂存区和工作目录都重置为指定的提交状态。

14. git config --global credential.helper store 
作用：配置 Git 记住你的用户名和密码，（通常是 GitHub、GitLab 等平台的账号密码），
避免每次推送或拉取时都要手动输入。

15. git checkout -- <file>
作用：丢弃某个文件在工作目录中的修改，恢复为最近一次提交的状态。

16. git reset HEAD <file> 
作用：取消暂存某个文件，即将它从暂存区移回工作目录，但保留文件内容不变。

17. git rm <file> 
作用：从 Git 仓库中删除一个文件，包括从工作目录和暂存区中同时删除。

18. git remote add origin git@github.com:michaelliao/learngit.git
作用：将一个远程仓库地址添加到本地 Git 项目中，并命名为 origin。

19. git push origin master
作用：将本地的 master 分支（主分支）内容推送到名为 origin 的远程仓库。

20. git remote -v
作用：查看当前配置的所有远程仓库地址，包括读写权限地址。

21. git remote rm origin
作用：从本地 Git 配置中删除名为 origin 的远程仓库。

22. git branch作用：列出当前所有的本地分支，并标记当前所在分支。

23. git branch <name>
作用：创建一个名为 <name> 的新分支（但不会自动切换过去）。

24. git checkout <name>
作用：切换到已存在的分支 <name>。

25. git switch <name>
作用：切换到已存在的分支 <name>（是 checkout 的更安全替代命令）。

26. git checkout -b <name>
作用：创建并切换到一个名为 <name> 的新分支（常用命令）。

27. git switch -c <name>
作用：创建并切换到一个名为 <name> 的新分支（推荐用于清晰操作意图）。

28. git merge <name>
作用：将指定分支 <name> 的更改合并到当前所在的分支中。

29. git branch -d <name>
作用：删除名为 <name> 的分支（如果该分支已合并到其他分支）。

30. git log --graph
作用：以图形化方式显示提交历史，展示分支和合并的结构。

31. git merge feature1
作用：将名为 feature1 的分支合并到当前所在分支中。

32. git push tbl --delete test3 
作用：在gitlab上面删除项目源为tbl并且分支名为test3的分支。

33. git clone -b test https://gitlab.eduxiji.net/T202511077996463/db123.git 
作用：在gitlab上面克隆分支名为test的项目内容。

```

**具体项目提交流程：**

```

1. git config --global user.name "Your Name" 
作用：设置 Git 的全局用户名。

2. git config --global user.email "email@example.com" 作用：设置 Git 的全局用户邮箱。

3. git config --global credential.helper store 
作用：配置 Git 记住你的用户名和密码（通常是 GitHub、GitLab 等平台的账号密码），
避免每次推送或拉取时都要手动输入。

4. git init 
作用：在当前目录下创建一个新的 Git 仓库。

5. git add . 
作用：将当前目录下的所有文件（包括子目录）添加到 Git 的暂存区（Staging Area）。

6. git commit -m "Initial commit" 
作用：将暂存区中的更改保存为一次提交（commit），并附上一条提交信息。

7. git status 
作用：查看当前仓库状态。

8. git remote -v
作用：查看当前配置的所有远程仓库地址，包括读写权限地址。

9. git remote add origin git@github.com:michaelliao/learngit.git
作用：将一个远程仓库地址添加到本地 Git 项目中，并命名为 origin。

10. git remote -v
作用：查看当前配置的所有远程仓库地址，包括读写权限地址。

11. git push origin master
作用：将本地的 master 分支（主分支）内容推送到名为 origin 的远程仓库。

```