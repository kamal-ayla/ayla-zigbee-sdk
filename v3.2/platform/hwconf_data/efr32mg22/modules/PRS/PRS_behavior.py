from . import ExporterModel
from . import RuntimeModel

PRS_CHANNEL_COUNT = 12
PRS_CHANNEL_GPIO_COUNT = 12
PRS_SOURCE_TO_SIGNALS_MAP = {
    'USART0': ['USART0_CS', 'USART0_IRTX', 'USART0_RTS', 'USART0_RXDATA', 'USART0_TX', 'USART0_TXC', ],
    'USART1': ['USART1_CS', 'USART1_IRTX', 'USART1_RTS', 'USART1_RXDATA', 'USART1_TX', 'USART1_TXC', ],
    'TIMER0': ['TIMER0_UF', 'TIMER0_OF', 'TIMER0_CC0', 'TIMER0_CC1', 'TIMER0_CC2', ],
    'TIMER1': ['TIMER1_UF', 'TIMER1_OF', 'TIMER1_CC0', 'TIMER1_CC1', 'TIMER1_CC2', ],
    'IADC0': ['IADC0_SCANENTRYDONE', 'IADC0_SCANTABLEDONE', 'IADC0_SINGLEDONE', ],
    'LETIMER0': ['LETIMER0_CH0', 'LETIMER0_CH1', ],
    'RTCC': ['RTCC_CCV0', 'RTCC_CCV1', 'RTCC_CCV2', ],
    'BURTC': ['BURTC_COMP', 'BURTC_OVERFLOW', ],
    'GPIO': ['GPIO_PIN0', 'GPIO_PIN1', 'GPIO_PIN2', 'GPIO_PIN3', 'GPIO_PIN4', 'GPIO_PIN5', 'GPIO_PIN6', 'GPIO_PIN7', ],
    'TIMER2': ['TIMER2_UF', 'TIMER2_OF', 'TIMER2_CC0', 'TIMER2_CC1', 'TIMER2_CC2', ],
    'TIMER3': ['TIMER3_UF', 'TIMER3_OF', 'TIMER3_CC0', 'TIMER3_CC1', 'TIMER3_CC2', ],
    'CORE': ['CORE_CTIOUT0', 'CORE_CTIOUT1', 'CORE_CTIOUT2', 'CORE_CTIOUT3', ],
    'CMUL': ['CMUL_CLKOUT0', 'CMUL_CLKOUT1', 'CMUL_CLKOUT2', ],
    'AGCL': ['AGCL_CCA', 'AGCL_CCAREQ', 'AGCL_GAINADJUST', 'AGCL_GAINOK', 'AGCL_GAINREDUCED', 'AGCL_IFPKI1', 'AGCL_IFPKQ2', 'AGCL_IFPKRST', ],
    'AGC': ['AGC_PEAKDET', 'AGC_PROPAGATED', 'AGC_RSSIDONE', ],
    'BUFC': ['BUFC_THR0', 'BUFC_THR1', 'BUFC_THR2', 'BUFC_THR3', 'BUFC_CNT0', 'BUFC_CNT1', 'BUFC_FULL', ],
    'MODEML': ['MODEML_ADVANCE', 'MODEML_ANT0', 'MODEML_ANT1', 'MODEML_COHDSADET', 'MODEML_COHDSALIVE', 'MODEML_DCLK', 'MODEML_DOUT', 'MODEML_FRAMEDET', ],
    'MODEM': ['MODEM_FRAMESENT', 'MODEM_LOWCORR', 'MODEM_LRDSADET', 'MODEM_LRDSALIVE', 'MODEM_NEWSYMBOL', 'MODEM_NEWWND', 'MODEM_POSTPONE', 'MODEM_PREDET', ],
    'MODEMH': ['MODEMH_PRESENT', 'MODEMH_RSSIJUMP', 'MODEMH_SYNCSENT', 'MODEMH_TIMDET', 'MODEMH_WEAK', 'MODEMH_EOF', ],
    'FRC': ['FRC_DCLK', 'FRC_DOUT', ],
    'PROTIMERL': ['PROTIMERL_BOF', 'PROTIMERL_CC0', 'PROTIMERL_CC1', 'PROTIMERL_CC2', 'PROTIMERL_CC3', 'PROTIMERL_CC4', 'PROTIMERL_LBTF', 'PROTIMERL_LBTR', ],
    'PROTIMER': ['PROTIMER_LBTS', 'PROTIMER_POF', 'PROTIMER_T0MATCH', 'PROTIMER_T0UF', 'PROTIMER_T1MATCH', 'PROTIMER_T1UF', 'PROTIMER_WOF', ],
    'SYNTH': ['SYNTH_MUX0', 'SYNTH_MUX1', ],
    'PRORTC': ['PRORTC_CCV0', 'PRORTC_CCV1', ],
    'PRSL': ['PRSL_ASYNCH0', 'PRSL_ASYNCH1', 'PRSL_ASYNCH2', 'PRSL_ASYNCH3', 'PRSL_ASYNCH4', 'PRSL_ASYNCH5', 'PRSL_ASYNCH6', 'PRSL_ASYNCH7', ],
    'PRS': ['PRS_ASYNCH8', 'PRS_ASYNCH9', 'PRS_ASYNCH10', 'PRS_ASYNCH11', ],
    'PDML': ['PDML_PDMDSRPULSE', ],
    'EUART0': ['EUART0_IRDATX', 'EUART0_RTS', 'EUART0_TX', 'EUART0_TXC', 'EUART0_RXFL', ],
    'RACL': ['RACL_ACTIVE', 'RACL_LNAEN', 'RACL_PAEN', 'RACL_RX', 'RACL_TX', 'RACL_CTIOUT0', 'RACL_CTIOUT1', 'RACL_CTIOUT2', ],
    'RAC': ['RAC_CTIOUT3', ],
    'TIMER4': ['TIMER4_UF', 'TIMER4_OF', 'TIMER4_CC0', 'TIMER4_CC1', 'TIMER4_CC2', ],
    'LFRCO': ['LFRCO_CALMEAS', 'LFRCO_SDM', 'LFRCO_TCMEAS', ],
}


class PRS(ExporterModel.Module):
    def __init__(self, name="PRS"):
        if not name:
            name = self.__class__.__name__
        super(PRS, self).__init__(name, visible=True)

    def load_halconfig_model(self, available_module_names_list, family):
        for i in range(PRS_CHANNEL_COUNT):
            # creating custom name object for each channel
            custom_name_prop = ExporterModel.StringProperty(name="custom_name_CH{}".format(i), description="Custom name", visible=True)
            custom_name_prop.subcategory = "CH{} properties".format(i)
            self.add_property(custom_name_prop)
            # creating property object for possible sources
            source_prop_obj = ExporterModel.EnumProperty(name="BSP_PRS_CH{}_SOURCE".format(i), description="Source", visible=True)
            source_prop_obj.add_enum("Disabled")
            source_prop_obj.subcategory = "CH{} properties".format(i)
            self.add_property(source_prop_obj)

            # Adding values to the drop downs
            for source, signals in PRS_SOURCE_TO_SIGNALS_MAP.items():
                source_prop_obj.add_enum(source)

                signal_prop_obj = ExporterModel.EnumProperty(name="BSP_PRS_CH{}_SIGNAL_{}".format(i, source), description="Signal", visible=False)
                for signal in signals:
                    signal_prop_obj.add_enum(signal)
                signal_prop_obj.subcategory = "CH{} properties".format(i)
                self.add_property(signal_prop_obj)

            # Adding readonly properties, to be be displayed when selected source option is "Disabled"
            disabled_signal_prop_obj = ExporterModel.StringProperty(name="bsp_prs_ch{0}_signal_disabled".format(i), description="Signal", visible=True, readonly=True)
            disabled_signal_prop_obj.subcategory = "CH{} properties".format(i)
            self.add_property(disabled_signal_prop_obj)
            if i < PRS_CHANNEL_GPIO_COUNT:
                pin_prop = ExporterModel.PinProperty(name="BSP_PRS_CH{}".format(i),
                                                     description="Output pin".format(i),
                                                     visible=True)

                pin_prop.set_reference("PRS", "ASYNCH{}".format(i))

                pin_prop.subcategory = "CH{} properties".format(i)
                self.add_property(pin_prop)

    def set_runtime_hooks(self):
        for i in range(PRS_CHANNEL_COUNT):
            RuntimeModel.set_change_handler(self.get_property("BSP_PRS_CH{}_SOURCE".format(i)), PRS.mode_changed)

    @staticmethod
    def mode_changed(studio_mod, property, state):
        new_source_name = RuntimeModel.get_property_define(property, studio_mod)
        print("PRS source on {} changed to {}".format(property.name, new_source_name))

        # Getting channel number
        chan = property.name.split("CH")[1]
        for i, char in enumerate(chan):
            if not char.isdigit():
                chan = chan[:i]
                break

        # Hiding and showing properties depending on source choice
        for mod_prop in [prop for prop in property.parent.get_properties() if prop.subcategory == property.subcategory]:
            mod_prop_name = mod_prop.name.upper()
            if "CH" + chan + "_SIGNAL" in mod_prop_name:
                if mod_prop_name.endswith("_" + new_source_name.upper()):
                    RuntimeModel.set_property_hidden(mod_prop, False, state=state)
                else:
                    RuntimeModel.set_property_hidden(mod_prop, True, state=state)
