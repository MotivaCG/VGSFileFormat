#include "taskqueuedialog.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
QString stateText(int state,const QString &message) {
    static const char *names[]={"Pending","Exporting","Done","Failed","Skipped","Canceled"};
    return message.isEmpty() ? QObject::tr(names[state]) : QObject::tr(names[state])+": "+message;
}
}

TaskQueueDialog::TaskQueueDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Process tasks"));setObjectName("taskQueue");setAcceptDrops(true);resize(900,460);
    auto *layout=new QVBoxLayout(this);
    list_=new QTreeWidget;list_->setObjectName("taskList");list_->setColumnCount(4);list_->setHeaderLabels({tr("Task"),tr("Output"),tr("Frames"),tr("Status")});
    list_->setRootIsDecorated(false);list_->setSelectionMode(QAbstractItemView::ExtendedSelection);list_->setAlternatingRowColors(true);
    list_->header()->setSectionResizeMode(0,QHeaderView::ResizeToContents);list_->header()->setSectionResizeMode(1,QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);list_->header()->setSectionResizeMode(3,QHeaderView::Stretch);
    list_->setToolTip(tr("The tasks in the queue. Add .vgstask files with Add tasks, or drop them here."));
    layout->addWidget(list_,1);
    auto *edit=new QHBoxLayout;
    add_=new QPushButton(tr("Add tasks…"));add_->setObjectName("addTasks");remove_=new QPushButton(tr("Remove"));remove_->setObjectName("removeTasks");
    remove_->setToolTip(tr("Remove the selected tasks from the queue.\nThe .vgstask files stay where they are."));
    edit->addWidget(add_);edit->addWidget(remove_);edit->addStretch();layout->addLayout(edit);
    taskLabel_=new QLabel(tr("Current task"));taskBar_=new QProgressBar;taskBar_->setObjectName("taskProgress");taskBar_->setRange(0,100);taskBar_->setValue(0);
    totalLabel_=new QLabel(tr("All tasks"));totalBar_=new QProgressBar;totalBar_->setObjectName("totalProgress");totalBar_->setRange(0,1000);totalBar_->setValue(0);totalBar_->setTextVisible(false);
    for (auto *label:{taskLabel_,totalLabel_}) label->setWordWrap(true);
    layout->addWidget(taskLabel_);layout->addWidget(taskBar_);layout->addWidget(totalLabel_);layout->addWidget(totalBar_);
    auto *buttons=new QHBoxLayout;
    start_=new QPushButton(tr("Start"));start_->setObjectName("startTasks");start_->setDefault(true);
    start_->setToolTip(tr("Export every pending task, one after another.\nExisting outputs are overwritten. A task that fails is reported and the queue goes on."));
    cancelTask_=new QPushButton(tr("Cancel task"));cancelTask_->setObjectName("cancelTask");cancelTask_->setToolTip(tr("Stop the task being exported and go on with the next."));
    cancelAll_=new QPushButton(tr("Cancel all"));cancelAll_->setObjectName("cancelAllTasks");cancelAll_->setToolTip(tr("Stop the task being exported and every one after it."));
    close_=new QPushButton(tr("Close"));
    buttons->addWidget(start_);buttons->addWidget(cancelTask_);buttons->addWidget(cancelAll_);buttons->addStretch();buttons->addWidget(close_);layout->addLayout(buttons);
    connect(add_,&QPushButton::clicked,this,[this] {
        QSettings settings;
        const auto paths=QFileDialog::getOpenFileNames(this,tr("Add tasks"),settings.value("Tasks/Directory").toString(),tr("VGS Editor tasks (*.vgstask)"));
        if (paths.isEmpty()) return;
        settings.setValue("Tasks/Directory",QFileInfo(paths.first()).absolutePath());addTasks(paths);
    });
    connect(remove_,&QPushButton::clicked,this,[this] {
        if (running()) return;
        QList<int> rows;for (auto *item:list_->selectedItems()) rows<<list_->indexOfTopLevelItem(item);
        std::sort(rows.begin(),rows.end(),std::greater<int>());
        for (int row:rows) {entries_.removeAt(row);delete list_->takeTopLevelItem(row);}
        refreshButtons();
    });
    connect(start_,&QPushButton::clicked,this,&TaskQueueDialog::start);
    connect(cancelTask_,&QPushButton::clicked,this,[this] {stopTask_=true;});
    connect(cancelAll_,&QPushButton::clicked,this,[this] {stopAll_=true;stopTask_=true;});
    connect(close_,&QPushButton::clicked,this,&QDialog::close);
    connect(list_,&QTreeWidget::itemSelectionChanged,this,&TaskQueueDialog::refreshButtons);
    refreshButtons();
}

TaskQueueDialog::~TaskQueueDialog() {
    if (job_) {stopAll_=true;stopTask_=true;job_->wait();delete job_;}
}

void TaskQueueDialog::addTasks(const QStringList &paths) {
    for (const auto &path:paths) {
        const QString absolute=QFileInfo(path).absoluteFilePath();
        if (std::any_of(entries_.cbegin(),entries_.cend(),[&](const Entry &e) {return e.task.path.compare(absolute,Qt::CaseInsensitive)==0;})) continue;
        Entry entry;QString error;
        if (!readExportTask(absolute,&entry.task,&error)) {entry.task.path=absolute;entry.state=State::Failed;entry.message=error;}
        entries_.append(entry);
        auto *item=new QTreeWidgetItem(list_);item->setText(0,QFileInfo(absolute).completeBaseName());item->setToolTip(0,QDir::toNativeSeparators(absolute));
        refreshRow(int(entries_.size())-1);
    }
    refreshButtons();
}

void TaskQueueDialog::refreshRow(int row) {
    auto *item=list_->topLevelItem(row);const auto &e=entries_[row];
    item->setText(1,QDir::toNativeSeparators(e.task.output));item->setToolTip(1,item->text(1));
    item->setText(2,e.task.frames>0 ? QString::number(e.task.frames) : QStringLiteral("?"));
    item->setText(3,stateText(int(e.state),e.message));item->setToolTip(3,item->text(3));
}

void TaskQueueDialog::refreshButtons() {
    const bool busy=running();
    const bool pending=std::any_of(entries_.cbegin(),entries_.cend(),[](const Entry &e) {return e.state==State::Pending;});
    add_->setEnabled(!busy);remove_->setEnabled(!busy && !list_->selectedItems().isEmpty());start_->setEnabled(!busy && pending);
    cancelTask_->setEnabled(busy);cancelAll_->setEnabled(busy);
}

void TaskQueueDialog::start() {
    if (running()) return;
    stopTask_=false;stopAll_=false;run_.clear();
    // Checked before anything runs, so a task that never could is known at once.
    for (int i=0;i<entries_.size();++i) {
        auto &e=entries_[i];if (e.state!=State::Pending) continue;
        const QString problem=checkExportTask(e.task);
        if (!problem.isEmpty()) {e.state=State::Skipped;e.message=problem;refreshRow(i);continue;}
        run_.append(i);
    }
    if (run_.isEmpty()) {refreshButtons();return;}
    awake_=std::make_unique<StayAwake>();runStarted_=QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    totalBar_->setValue(0);runNext();
}

void TaskQueueDialog::runNext() {
    current_=-1;
    if (!stopAll_) for (int i:run_) if (entries_[i].state==State::Pending) {current_=i;break;}
    if (current_<0) {
        for (int i:run_) if (entries_[i].state==State::Pending) {entries_[i].state=State::Cancelled;refreshRow(i);}
        awake_.reset();writeLog();
        int done=0,failed=0,other=0;for (int i:run_) {const auto s=entries_[i].state;s==State::Done ? ++done : s==State::Failed ? ++failed : ++other;}
        taskLabel_->setText(tr("Current task"));taskBar_->setValue(0);
        totalLabel_->setText(tr("Finished: %1 done, %2 failed, %3 canceled or skipped.").arg(done).arg(failed).arg(other));
        if (!stopAll_) totalBar_->setValue(totalBar_->maximum());
        refreshButtons();return;
    }
    auto &e=entries_[current_];e.state=State::Running;e.message.clear();refreshRow(current_);
    stopTask_=false;jobFailure_.clear();jobResult_={};taskBar_->setValue(0);showProgress(0,tr("Starting"));
    const ExportTask task=e.task;
    job_=QThread::create([this,task] {
        QElapsedTimer clock;clock.start();
        try {
            jobResult_=runExportTask(task,[this](int percent,const QString &message) {
                QMetaObject::invokeMethod(this,[this,percent,message] {showProgress(percent,message);},Qt::QueuedConnection);
                return !stopTask_.load();
            });
        } catch (const std::exception &error) {jobFailure_=QString::fromUtf8(error.what());}
        if (jobFailure_.isEmpty() && stopTask_) jobFailure_=QStringLiteral("Export canceled.");
    });
    connect(job_,&QThread::finished,this,[this] {finished(jobFailure_);});
    refreshButtons();job_->start();
}

void TaskQueueDialog::finished(const QString &failure) {
    job_->wait();delete job_;job_=nullptr;
    auto &e=entries_[current_];
    if (failure.isEmpty()) {
        e.state=State::Done;e.result=jobResult_;
        e.message=tr("%1 frames, %2 MB").arg(jobResult_.frames).arg(QFileInfo(e.task.output).size()/1e6,0,'f',1);
    } else if (stopTask_) {e.state=State::Cancelled;e.message.clear();}
    else {e.state=State::Failed;e.message=failure;}
    refreshRow(current_);showProgress(100,QString());runNext();
}

void TaskQueueDialog::showProgress(int percent,const QString &message) {
    if (current_<0) return;
    const auto &e=entries_[current_];percent=std::clamp(percent,0,100);
    taskBar_->setValue(percent);
    taskLabel_->setText(tr("%1 → %2%3").arg(QFileInfo(e.task.path).completeBaseName(),QFileInfo(e.task.output).fileName(),message.isEmpty() ? QString() : "\n"+message));
    // By frames when every task in the run says how many it exports, by task otherwise.
    const bool byFrames=std::all_of(run_.cbegin(),run_.cend(),[this](int i) {return entries_[i].task.frames>0;});
    double done=0,total=0;int position=0;
    for (int k=0;k<run_.size();++k) {
        const int i=run_[k];const double weight=byFrames ? entries_[i].task.frames : 1;total+=weight;
        if (i==current_) {done+=weight*percent/100.0;position=k+1;}
        else if (entries_[i].state!=State::Pending) done+=weight;
    }
    totalBar_->setValue(total>0 ? int(totalBar_->maximum()*done/total) : 0);
    totalLabel_->setText(byFrames ? tr("Task %1 of %2 · %3 of %4 frames").arg(position).arg(run_.size()).arg(qRound(done)).arg(qRound(total))
                                  : tr("Task %1 of %2").arg(position).arg(run_.size()));
}

// One log per run, beside the first task: what each task did, and why when it did not.
void TaskQueueDialog::writeLog() {
    if (run_.isEmpty()) return;
    const QString path=QFileInfo(entries_[run_.first()].task.path).absolutePath()+"/vgstasks-"+runStarted_+".log";
    QString text=QStringLiteral("VGS Editor task run %1\n\n").arg(runStarted_);
    for (int i:run_) {
        const auto &e=entries_[i];
        text+=QStringLiteral("%1  %2\n  output %3\n").arg(stateText(int(e.state),QString()),QDir::toNativeSeparators(e.task.path),QDir::toNativeSeparators(e.task.output));
        if (e.state==State::Done) {
            text+=QStringLiteral("  %1 frames, %2 kept, %3 removed, %4 bytes\n").arg(e.result.frames).arg(e.result.kept).arg(e.result.removed).arg(QFileInfo(e.task.output).size());
            for (const auto &note:e.result.notes) text+="  "+note+"\n";
        } else if (!e.message.isEmpty()) text+="  "+e.message+"\n";
        text+="\n";
    }
    QSaveFile file(path);const auto bytes=text.toUtf8();
    if (file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size()) file.commit();
}

void TaskQueueDialog::closeEvent(QCloseEvent *event) {
    // Closing stops the queue: the task being exported is cancelled and leaves no file.
    if (job_) {stopAll_=true;stopTask_=true;job_->wait();delete job_;job_=nullptr;awake_.reset();}
    QDialog::closeEvent(event);
}

void TaskQueueDialog::dragEnterEvent(QDragEnterEvent *event) {
    if (running() || !event->mimeData()->hasUrls()) return;
    for (const auto &url:event->mimeData()->urls()) if (url.toLocalFile().endsWith(".vgstask",Qt::CaseInsensitive)) {event->acceptProposedAction();return;}
}

void TaskQueueDialog::dropEvent(QDropEvent *event) {
    QStringList paths;for (const auto &url:event->mimeData()->urls()) if (url.toLocalFile().endsWith(".vgstask",Qt::CaseInsensitive)) paths<<url.toLocalFile();
    addTasks(paths);event->acceptProposedAction();
}
