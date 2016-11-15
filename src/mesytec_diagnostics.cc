#include "mesytec_diagnostics.h"
#include "ui_mesytec_diagnostics.h"
#include "realtimedata.h"
#include "hist1d.h"

#include <QDebug>
#include <QTimer>

//
// MesytecDiagnostics
//
#define MAXIDX 40
#define MINIDX 41
#define ODD 42
#define EVEN 43
#define MAXFILT 44
#define MINFILT 45
#define ODDFILT 46
#define EVENFILT 47

static const int histoCount = 34;
static const int histoBits = 13;
static const int dataExtractMask = 0x00001FFF;

MesytecDiagnostics::MesytecDiagnostics(QObject *parent)
    : QObject(parent)
    , m_rtd(new RealtimeData(this))
{
    for (int i=0; i<histoCount; ++i)
    {
        m_histograms.push_back(new Hist1D(histoBits, this));
    }
}

void MesytecDiagnostics::setEventAndModuleIndices(const QPair<int, int> &indices)
{
    qDebug() << __PRETTY_FUNCTION__ << indices;
    m_eventIndex = indices.first;
    m_moduleIndex = indices.second;
}

void MesytecDiagnostics::handleDataWord(quint32 currentWord)
{
    if (currentWord == 0xFFFFFFFF || currentWord == 0x00000000)
        return;

    bool data_found_flag = ((currentWord & 0xF0000000) == 0x10000000) // MDPP
        || ((currentWord & 0xFF800000) == 0x04000000); // MxDC

    if (data_found_flag)
    {
        u16 channel = (currentWord & 0x003F0000) >> 16; // 6 bit address
        u32 value   = (currentWord & dataExtractMask);

        if (channel < m_histograms.size())
        {
            m_histograms[channel]->fill(value);
        }

        m_rtd->insertData(channel, value);
    }
}

void MesytecDiagnostics::clear(void)
{
    quint16 i;

    mean[0] = 0;
    for(i=0;i<50;i++){
        mean[i] = 0;
        sigma[i] = 0;
        meanchannel[i] = 0;
        sigmachannel[i] = 0;
        max[i] = 0;
        maxchan[i] = 0;
        counts[i] = 0;
    }
    // set minima to hi values
    mean[MINIDX] = 128000;
    sigma[MINIDX] = 128000;
    mean[MINFILT] = 128000;
    sigma[MINFILT] = 128000;
}

void MesytecDiagnostics::calcAll(quint16 lo, quint16 hi, quint16 lo2, quint16 hi2, quint16 binLo, quint16 binHi)
{
    quint16 i, j;
    quint16 evencounts = 0, evencounts2 = 0, oddcounts = 0, oddcounts2 = 0;
    double dval;
    //quint32 res = 1 << histoBits;
    qDebug("%d %d", binLo, binHi);
    //reset all old calculations
    clear();

    // iterate through all channels (34 real channels max.)
    for(i=0; i<34; i++){
        // calculate means and maxima
        for(j=binLo; j<=binHi; j++){
            auto value = m_histograms[i]->value(j);

            mean[i] += value * j;
            counts[i] += value;

            if (value > max[i])
            {
                maxchan[i] = j;
                max[i] = value;
            }
        }

        if(counts[i])
            mean[i] /= counts[i];
        else
            mean[i] = 0;

        if(mean[i]){
            // calculate sigmas
            for(j=binLo; j<=binHi; j++){
                dval =  j - mean[i];
                dval *= dval;
                dval *= m_histograms[i]->value(j);
                //dval *= p_myHist->m_data[i*res + j];
                sigma[i] += dval;
            }
            if(counts[i])
                sigma[i] = sqrt(sigma[i]/counts[i]);
            else
                sigma[i] = 0;
        }
    }

    // find max and min mean and sigma
    for(i=0; i<34; i++){
        if(i>=lo && i <= hi){
            if(mean[i] > mean[MAXIDX]){
                mean[MAXIDX] = mean[i];
                meanchannel[MAXIDX] = i;
            }
            if(mean[i] < mean[MINIDX]){
                mean[MINIDX] = mean[i];
                meanchannel[MINIDX] = i;
            }
            if(sigma[i] > sigma[MAXIDX]){
                sigma[MAXIDX] = sigma[i];
                sigmachannel[MAXIDX] = i;
            }
            if(sigma[i] < sigma[MINIDX]){
                sigma[MINIDX] = sigma[i];
                sigmachannel[MINIDX] = i;
            }
        }
        if(i>=lo2 && i <= hi2){
            if(mean[i] > mean[MAXFILT]){
                mean[MAXFILT] = mean[i];
                meanchannel[MAXFILT] = i;
            }
            if(mean[i] < mean[MINFILT]){
                mean[MINFILT] = mean[i];
                meanchannel[MINFILT] = i;
            }
            if(sigma[i] > sigma[MAXFILT]){
                sigma[MAXFILT] = sigma[i];
                sigmachannel[MAXFILT] = i;
            }
            if(sigma[i] < sigma[MINFILT]){
                sigma[MINFILT] = sigma[i];
                sigmachannel[MINFILT] = i;
            }
        }
    }

    // now odds and evens
    for(i=0; i<34; i++){
        // calculate means and maxima
        // odd?
        if(i%2){
            if(i>=lo && i <= hi){
                mean[ODD] += mean[i];
                counts[ODD] += counts[i];
                oddcounts++;
            }
            if(i>=lo2 && i <= hi2){
                mean[ODDFILT] += mean[i];
                counts[ODDFILT] += counts[i];
                oddcounts2++;
            }
        }
        else{
            if(i>=lo && i <= hi){
                mean[EVEN] += mean[i];
                counts[EVEN] += counts[i];
                evencounts++;
            }
            if(i>=lo2 && i <= hi2){
                mean[EVENFILT] += mean[i];
                counts[EVENFILT] += counts[i];
                evencounts2++;
            }
        }
    }
    mean[EVEN] /= evencounts;
    mean[ODD] /= oddcounts;
    mean[EVENFILT] /= evencounts2;
    mean[ODDFILT] /= oddcounts2;

    // calculate sigmas
    for(i=0; i<34; i++){
        for(j=binLo; j<=binHi; j++){
            dval =  j - mean[i];
            dval *= dval,
            dval *= m_histograms[i]->value(j);
            //dval *= p_myHist->m_data[i*res + j];
            if(i%2){
                if(i>=lo && i <= hi)
                    sigma[ODD] += dval;
                if(i>=lo2 && i <= hi2)
                    sigma[ODDFILT] += dval;
            }
            else{
                if(i>=lo && i <= hi)
                    sigma[EVEN] += dval;
                if(i>=lo2 && i <= hi2)
                    sigma[EVENFILT] += dval;
            }
        }
    }
//    qDebug("%2.2f, %2.2f, %2.2f, %2.2f, %2.2f, %2.2f", counts[EVEN], sigma[EVEN], counts[ODD], sigma[ODD], sigma[32], sigma[33]);

    if(counts[EVEN])
        sigma[EVEN] = sqrt(sigma[EVEN]/counts[EVEN]);
    else
        sigma[EVEN] = 0;

    if(counts[ODD])
        sigma[ODD] = sqrt(sigma[ODD]/counts[ODD]);
    else
        sigma[ODD] = 0;

    if(counts[EVENFILT])
        sigma[EVENFILT] = sqrt(sigma[EVENFILT]/counts[EVENFILT]);
    else
        sigma[EVENFILT] = 0;

    if(counts[ODDFILT])
        sigma[ODDFILT] = sqrt(sigma[ODDFILT]/counts[ODDFILT]);
    else
        sigma[ODDFILT] = 0;

}

double MesytecDiagnostics::getMean(quint16 chan)
{
    return mean[chan];
}

double MesytecDiagnostics::getSigma(quint16 chan)
{
    return sigma[chan];
}

quint32 MesytecDiagnostics::getMeanchannel(quint16 chan)
{
    return meanchannel[chan];
}

quint32 MesytecDiagnostics::getSigmachannel(quint16 chan)
{
    return sigmachannel[chan];
}

quint32 MesytecDiagnostics::getMax(quint16 chan)
{
    return max[chan];
}

quint32 MesytecDiagnostics::getMaxchan(quint16 chan)
{
    return maxchan[chan];
}

quint32 MesytecDiagnostics::getCounts(quint16 chan)
{
    return counts[chan];
}

quint32 MesytecDiagnostics::getChannel(quint16 chan, quint32 bin)
{
    return m_histograms[chan]->value(bin);
}

//
// MesytecDiagnosticsWidget
//

static const int updateInterval = 500;

MesytecDiagnosticsWidget::MesytecDiagnosticsWidget(MesytecDiagnostics *diag, QWidget *parent)
    : MVMEWidget(parent)
    , ui(new Ui::DiagnosticsWidget)
    , m_diag(diag)
{
    ui->setupUi(this);
    auto updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, &MesytecDiagnosticsWidget::updateRtDisplay);

    updateTimer->setInterval(updateInterval);
    updateTimer->start();
}

MesytecDiagnosticsWidget::~MesytecDiagnosticsWidget()
{
    delete ui;
}

void MesytecDiagnosticsWidget::on_calcAll_clicked()
{
    QString str;
    quint16 lobin = 0, hibin = 8192;
    // evaluate bin filter
    if(ui->bin1->isChecked()){
        lobin = ui->binRange1lo->value();
        hibin = ui->binRange1hi->value();
    }
    if(ui->bin2->isChecked()){
        lobin = ui->binRange2lo->value();
        hibin = ui->binRange2hi->value();
    }
    if(ui->bin3->isChecked()){
        lobin = ui->binRange3lo->value();
        hibin = ui->binRange3hi->value();
    }

    m_diag->calcAll(ui->diagLowChannel2->value(), ui->diagHiChannel2->value(),
                    ui->diagLowChannel->value(), ui->diagHiChannel->value(),
                    lobin, hibin);

    dispAll();
}

void MesytecDiagnosticsWidget::on_diagBin_valueChanged(int value)
{
    dispChan();
}

void MesytecDiagnosticsWidget::on_diagChan_valueChanged(int value)
{
    dispChan();
}

void MesytecDiagnosticsWidget::dispAll()
{
    dispDiag1();
    dispDiag2();
    dispResultList();
}

void MesytecDiagnosticsWidget::dispDiag1()
{
    QString str;
    // upper range
    str.sprintf("%2.2f", m_diag->getMean(MAXIDX));
    ui->meanmax->setText(str);
    str.sprintf("%d", m_diag->getMeanchannel(MAXIDX));
    ui->meanmaxchan->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(MAXIDX));
    ui->sigmax->setText(str);
    str.sprintf("%d", m_diag->getSigmachannel(MAXIDX));
    ui->sigmaxchan->setText(str);
    str.sprintf("%2.2f", m_diag->getMean(MINIDX));
    ui->meanmin->setText(str);
    str.sprintf("%d", m_diag->getMeanchannel(MINIDX));
    ui->meanminchan->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(MINIDX));
    ui->sigmin->setText(str);
    str.sprintf("%d", m_diag->getSigmachannel(MINIDX));
    ui->sigminchan->setText(str);

    // odd even values upper range
    str.sprintf("%2.2f", m_diag->getMean(ODD));
    ui->meanodd->setText(str);
    str.sprintf("%2.2f", m_diag->getMean(EVEN));
    ui->meaneven->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(ODD));
    ui->sigmodd->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(EVEN));
    ui->sigmeven->setText(str);
}

void MesytecDiagnosticsWidget::dispDiag2()
{
    QString str;
    // lower range
    str.sprintf("%2.2f", m_diag->getMean(MAXFILT));
    ui->meanmax_filt->setText(str);
    str.sprintf("%d", m_diag->getMeanchannel(MAXFILT));
    ui->meanmaxchan_filt->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(MAXFILT));
    ui->sigmax_filt->setText(str);
    str.sprintf("%d", m_diag->getSigmachannel(MAXFILT));
    ui->sigmaxchan_filt->setText(str);
    str.sprintf("%2.2f", m_diag->getMean(MINFILT));
    ui->meanmin_filt->setText(str);
    str.sprintf("%d", m_diag->getMeanchannel(MINFILT));
    ui->meanminchan_filt->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(MINFILT));
    ui->sigmin_filt->setText(str);
    str.sprintf("%d", m_diag->getSigmachannel(MINFILT));
    ui->sigminchan_filt->setText(str);

    // odd even values lower range
    str.sprintf("%2.2f", m_diag->getMean(ODDFILT));
    ui->meanodd_filt->setText(str);
    str.sprintf("%2.2f", m_diag->getMean(EVENFILT));
    ui->meaneven_filt->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(ODDFILT));
    ui->sigmodd_filt->setText(str);
    str.sprintf("%2.2f", m_diag->getSigma(EVENFILT));
    ui->sigmeven_filt->setText(str);
}

void MesytecDiagnosticsWidget::dispResultList()
{
    QString text, str;
    quint16 i;

    for(i=0;i<34;i++){
        str.sprintf("%d:\t mean: %2.2f,\t sigma: %2.2f,\t\t counts: %d\n", i,
                    m_diag->getMean(i),
                    m_diag->getSigma(i),
                    m_diag->getCounts(i));
        text.append(str);
    }
    ui->diagResult->setPlainText(text);
}

void MesytecDiagnosticsWidget::dispChan()
{
    QString str;
    str.sprintf("%d", m_diag->getChannel(ui->diagChan->value(), ui->diagBin->value()));
    ui->diagCounts->setText(str);
}

void MesytecDiagnosticsWidget::dispRt()
{
    auto rtd = m_diag->getRealtimeData();
    QString str;
    str.sprintf("%2.2f", rtd->getRdMean(0));
    ui->rtMeanEven->setText(str);
    str.sprintf("%2.2f", rtd->getRdMean(1));
    ui->rtMeanOdd->setText(str);
    str.sprintf("%2.2f", rtd->getRdSigma(0));
    ui->rtSigmEven->setText(str);
    str.sprintf("%2.2f", rtd->getRdSigma(1));
    ui->rtSigmOdd->setText(str);
}

void MesytecDiagnosticsWidget::updateRtDisplay()
{
    m_diag->getRealtimeData()->calcData();
    dispRt();
}
