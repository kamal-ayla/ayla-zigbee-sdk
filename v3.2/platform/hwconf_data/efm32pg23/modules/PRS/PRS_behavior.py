from . import ExporterModel
from . import RuntimeModel

PRS_CHANNEL_COUNT = 12
PRS_CHANNEL_GPIO_COUNT = 12
PRS_SOURCE_TO_SIGNALS_MAP = {
    'USART0': ['USART0_CS', 'USART0_IRTX', 'USART0_RTS', 'USART0_RXDATA', 'USART0_TX', 'USART0_TXC', ],
    'TIMER0': ['TIMER0_UF', 'TIMER0_OF', 'TIMER0_CC0', 'TIMER0_CC1', 'TIMER0_CC2', ],
    'TIMER1': ['TIMER1_UF', 'TIMER1_OF', 'TIMER1_CC0', 'TIMER1_CC1', 'TIMER1_CC2', ],
    'IADC0': ['IADC0_SCANENTRYDONE', 'IADC0_SCANTABLEDONE', 'IADC0_SINGLEDONE', ],
    'LETIMER0': ['LETIMER0_CH0', 'LETIMER0_CH1', ],
    'BURTC': ['BURTC_COMP', 'BURTC_OVERFLOW', ],
    'GPIO': ['GPIO_PIN0', 'GPIO_PIN1', 'GPIO_PIN2', 'GPIO_PIN3', 'GPIO_PIN4', 'GPIO_PIN5', 'GPIO_PIN6', 'GPIO_PIN7', ],
    'TIMER2': ['TIMER2_UF', 'TIMER2_OF', 'TIMER2_CC0', 'TIMER2_CC1', 'TIMER2_CC2', ],
    'TIMER3': ['TIMER3_UF', 'TIMER3_OF', 'TIMER3_CC0', 'TIMER3_CC1', 'TIMER3_CC2', ],
    'CORE': ['CORE_CTIOUT0', 'CORE_CTIOUT1', 'CORE_CTIOUT2', 'CORE_CTIOUT3', ],
    'CMUL': ['CMUL_CLKOUT0', 'CMUL_CLKOUT1', 'CMUL_CLKOUT2', ],
    'PRSL': ['PRSL_ASYNCH0', 'PRSL_ASYNCH1', 'PRSL_ASYNCH2', 'PRSL_ASYNCH3', 'PRSL_ASYNCH4', 'PRSL_ASYNCH5', 'PRSL_ASYNCH6', 'PRSL_ASYNCH7', ],
    'PRS': ['PRS_ASYNCH8', 'PRS_ASYNCH9', 'PRS_ASYNCH10', 'PRS_ASYNCH11', ],
    'TIMER4': ['TIMER4_UF', 'TIMER4_OF', 'TIMER4_CC0', 'TIMER4_CC1', 'TIMER4_CC2', ],
    'ACMP0': ['ACMP0_OUT', ],
    'ACMP1': ['ACMP1_OUT', ],
    'VDAC0L': ['VDAC0L_CH0WARM', 'VDAC0L_CH1WARM', 'VDAC0L_CH0DONEASYNC', 'VDAC0L_CH1DONEASYNC', 'VDAC0L_INTERNALTIMEROF', 'VDAC0L_REFRESHTIMEROF', ],
    'PCNT0': ['PCNT0_DIR', 'PCNT0_UFOF', ],
    'SYSRTC0': ['SYSRTC0_GRP0OUT0', 'SYSRTC0_GRP0OUT1', 'SYSRTC0_GRP1OUT0', 'SYSRTC0_GRP1OUT1', ],
    'LESENSE': ['LESENSE_DECOUT0', 'LESENSE_DECOUT1', 'LESENSE_DECOUT2', 'LESENSE_DECCMP', ],
    'HFXO0L': ['HFXO0L_STATUS', 'HFXO0L_STATUS1', ],
    'EUSART0L': ['EUSART0L_CS', 'EUSART0L_IRDATX', 'EUSART0L_RTS', 'EUSART0L_RXDATAV', 'EUSART0L_TX', 'EUSART0L_TXC', 'EUSART0L_RXFL', 'EUSART0L_TXFL', ],
    'EUSART1L': ['EUSART1L_CS', 'EUSART1L_IRDATX', 'EUSART1L_RTS', 'EUSART1L_RXDATAV', 'EUSART1L_TX', 'EUSART1L_TXC', 'EUSART1L_RXFL', 'EUSART1L_TXFL', ],
    'EUSART2L': ['EUSART2L_CS', 'EUSART2L_IRDATX', 'EUSART2L_RTS', 'EUSART2L_RXDATAV', 'EUSART2L_TX', 'EUSART2L_TXC', 'EUSART2L_RXFL', 'EUSART2L_TXFL', ],
    'HFRCO0': ['HFRCO0_COREEN', 'HFRCO0_STATE0', 'HFRCO0_STATE1', 'HFRCO0_STATE2', ],
    'HFRCOEM23': ['HFRCOEM23_COREEN', 'HFRCOEM23_STATE0', 'HFRCOEM23_STATE1', 'HFRCOEM23_STATE2', ],
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